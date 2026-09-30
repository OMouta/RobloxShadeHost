// macOS: windows come from CoreGraphics, pictures from ScreenCaptureKit, which copies one window even where
// other windows cover it, and shortcuts from Carbon's hot keys, which work without accessibility permission.

#include "platform.h"
#include "log.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <libproc.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <thread>

extern char** environ;

namespace
{
struct CaptureState
{
    std::mutex mutex;
    platform::Frame ready;
    platform::Frame back;
    uint64_t serial = 0;
    std::string error;
    std::atomic<bool> running = false;
    std::atomic<uint64_t> generation = 0; // a new capture makes callbacks of the old one stale
    uint32_t configWidth = 0;
    uint32_t configHeight = 0;
    bool reconfiguring = false;
};
CaptureState capture;

void FailCapture(uint64_t generation, const std::string& message)
{
    if (generation != capture.generation)
        return;
    {
        std::lock_guard lock(capture.mutex);
        capture.error = message;
    }
    capture.running = false;
    glfwPostEmptyEvent();
}
} // namespace

@interface UnishadeStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) uint64_t generation;
@property(nonatomic, strong) SCStream* stream;
@end

@implementation UnishadeStreamOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || self.generation != capture.generation || !CMSampleBufferIsValid(sampleBuffer))
        return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (!attachments || CFArrayGetCount(attachments) == 0)
        return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    // Idle frames repeat the last one, which the host already has.
    if (!status || status.integerValue != SCFrameStatusComplete)
        return;
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!image)
        return;

    const size_t bufferWidth = CVPixelBufferGetWidth(image), bufferHeight = CVPixelBufferGetHeight(image);
    // The window's part of the frame. It is smaller than the frame while the stream catches up with a resize.
    size_t x0 = 0, y0 = 0, width = bufferWidth, height = bufferHeight;
    CGRect content = CGRectZero;
    NSDictionary* rect = info[SCStreamFrameInfoContentRect];
    const double scaleFactor = [info[SCStreamFrameInfoScaleFactor] doubleValue];
    const double contentScale = [info[SCStreamFrameInfoContentScale] doubleValue];
    if (rect && scaleFactor > 0 && CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)rect, &content))
    {
        x0 = std::min<size_t>(bufferWidth - 1, size_t(std::lround(content.origin.x * scaleFactor)));
        y0 = std::min<size_t>(bufferHeight - 1, size_t(std::lround(content.origin.y * scaleFactor)));
        width = std::clamp<size_t>(size_t(std::lround(content.size.width * scaleFactor)), 1, bufferWidth - x0);
        height = std::clamp<size_t>(size_t(std::lround(content.size.height * scaleFactor)), 1, bufferHeight - y0);

        // The window's real size in pixels. A stream the wrong size scales the window, so it is set again.
        if (contentScale > 0)
        {
            const uint32_t wantedWidth = uint32_t(std::lround(content.size.width / contentScale * scaleFactor));
            const uint32_t wantedHeight = uint32_t(std::lround(content.size.height / contentScale * scaleFactor));
            bool reconfigure = false;
            {
                std::lock_guard lock(capture.mutex);
                reconfigure = !capture.reconfiguring && wantedWidth > 0 && wantedHeight > 0 &&
                              (wantedWidth != capture.configWidth || wantedHeight != capture.configHeight);
                capture.reconfiguring |= reconfigure;
            }
            if (reconfigure)
            {
                SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
                config.width = wantedWidth;
                config.height = wantedHeight;
                config.pixelFormat = kCVPixelFormatType_32BGRA;
                config.showsCursor = NO;
                config.minimumFrameInterval = CMTimeMake(1, 240);
                config.queueDepth = 5;
                config.colorSpaceName = kCGColorSpaceSRGB;
                if (@available(macOS 14.0, *))
                    config.ignoreShadowsSingleWindow = YES;
                const uint64_t generation = self.generation;
                [stream updateConfiguration:config
                          completionHandler:^(NSError* error) {
                            std::lock_guard lock(capture.mutex);
                            if (generation != capture.generation)
                                return;
                            capture.reconfiguring = false;
                            if (!error)
                            {
                                capture.configWidth = wantedWidth;
                                capture.configHeight = wantedHeight;
                            }
                          }];
            }
        }
    }

    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
    const size_t stride = CVPixelBufferGetBytesPerRow(image);
    platform::Frame& back = capture.back;
    if (base && CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_32BGRA)
    {
        back.width = uint32_t(width);
        back.height = uint32_t(height);
        back.pixels.resize(width * height);
        for (size_t y = 0; y < height; ++y)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(base + (y0 + y) * stride) + x0;
            uint32_t* out = back.pixels.data() + y * width;
            for (size_t x = 0; x < width; ++x)
                out[x] = row[x] | 0xFF000000u;
        }
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    if (back.pixels.empty())
        return;
    {
        std::lock_guard lock(capture.mutex);
        back.serial = ++capture.serial;
        std::swap(back, capture.ready);
    }
    glfwPostEmptyEvent();
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    FailCapture(self.generation, std::string("Capture stopped: ") + error.localizedDescription.UTF8String);
}
@end

namespace platform
{
namespace
{
UnishadeStreamOutput* output = nil;
dispatch_queue_t captureQueue = nullptr;

HotkeyCallback hotkeyCallback;
std::map<int, EventHotKeyRef> hotkeys;
std::vector<std::pair<int, bool>> pendingHotkeys;
EventHandlerRef hotkeyHandler = nullptr;

NSArray* WindowList(CGWindowListOption options, CGWindowID window)
{
    return CFBridgingRelease(CGWindowListCopyWindowInfo(options, window));
}

bool Bounds(NSDictionary* info, Rect& bounds)
{
    CGRect rect;
    if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &rect))
        return false;
    bounds = { int(std::lround(rect.origin.x)), int(std::lround(rect.origin.y)), int(std::lround(rect.size.width)),
               int(std::lround(rect.size.height)) };
    return true;
}

std::string ProcessArgument(int pid)
{
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid };
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int))
        return {};
    std::vector<char> buffer(size);
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0)
        return {};
    // argc, then the executable path, padding, then the arguments.
    size_t at = sizeof(int);
    while (at < size && buffer[at])
        ++at;
    while (at < size && !buffer[at])
        ++at;
    size_t end = at;
    while (end < size && buffer[end])
        ++end;
    return at < size ? std::string(buffer.data() + at, end - at) : std::string();
}

// Carbon key codes follow the keyboard's layout, not the alphabet.
UInt32 KeyCode(ImGuiKey key)
{
    static const std::map<ImGuiKey, UInt32> codes = {
        { ImGuiKey_A, kVK_ANSI_A }, { ImGuiKey_B, kVK_ANSI_B }, { ImGuiKey_C, kVK_ANSI_C }, { ImGuiKey_D, kVK_ANSI_D },
        { ImGuiKey_E, kVK_ANSI_E }, { ImGuiKey_F, kVK_ANSI_F }, { ImGuiKey_G, kVK_ANSI_G }, { ImGuiKey_H, kVK_ANSI_H },
        { ImGuiKey_I, kVK_ANSI_I }, { ImGuiKey_J, kVK_ANSI_J }, { ImGuiKey_K, kVK_ANSI_K }, { ImGuiKey_L, kVK_ANSI_L },
        { ImGuiKey_M, kVK_ANSI_M }, { ImGuiKey_N, kVK_ANSI_N }, { ImGuiKey_O, kVK_ANSI_O }, { ImGuiKey_P, kVK_ANSI_P },
        { ImGuiKey_Q, kVK_ANSI_Q }, { ImGuiKey_R, kVK_ANSI_R }, { ImGuiKey_S, kVK_ANSI_S }, { ImGuiKey_T, kVK_ANSI_T },
        { ImGuiKey_U, kVK_ANSI_U }, { ImGuiKey_V, kVK_ANSI_V }, { ImGuiKey_W, kVK_ANSI_W }, { ImGuiKey_X, kVK_ANSI_X },
        { ImGuiKey_Y, kVK_ANSI_Y }, { ImGuiKey_Z, kVK_ANSI_Z },
        { ImGuiKey_0, kVK_ANSI_0 }, { ImGuiKey_1, kVK_ANSI_1 }, { ImGuiKey_2, kVK_ANSI_2 }, { ImGuiKey_3, kVK_ANSI_3 },
        { ImGuiKey_4, kVK_ANSI_4 }, { ImGuiKey_5, kVK_ANSI_5 }, { ImGuiKey_6, kVK_ANSI_6 }, { ImGuiKey_7, kVK_ANSI_7 },
        { ImGuiKey_8, kVK_ANSI_8 }, { ImGuiKey_9, kVK_ANSI_9 },
        { ImGuiKey_F1, kVK_F1 }, { ImGuiKey_F2, kVK_F2 }, { ImGuiKey_F3, kVK_F3 }, { ImGuiKey_F4, kVK_F4 }, { ImGuiKey_F5, kVK_F5 },
        { ImGuiKey_F6, kVK_F6 }, { ImGuiKey_F7, kVK_F7 }, { ImGuiKey_F8, kVK_F8 }, { ImGuiKey_F9, kVK_F9 }, { ImGuiKey_F10, kVK_F10 },
        { ImGuiKey_F11, kVK_F11 }, { ImGuiKey_F12, kVK_F12 }, { ImGuiKey_F13, kVK_F13 }, { ImGuiKey_F14, kVK_F14 }, { ImGuiKey_F15, kVK_F15 },
        { ImGuiKey_F16, kVK_F16 }, { ImGuiKey_F17, kVK_F17 }, { ImGuiKey_F18, kVK_F18 }, { ImGuiKey_F19, kVK_F19 }, { ImGuiKey_F20, kVK_F20 },
        { ImGuiKey_Home, kVK_Home }, { ImGuiKey_End, kVK_End }, { ImGuiKey_Insert, kVK_Help }, { ImGuiKey_Delete, kVK_ForwardDelete },
        { ImGuiKey_PageUp, kVK_PageUp }, { ImGuiKey_PageDown, kVK_PageDown }, { ImGuiKey_Space, kVK_Space }, { ImGuiKey_Tab, kVK_Tab },
        { ImGuiKey_Escape, kVK_Escape }, { ImGuiKey_LeftArrow, kVK_LeftArrow }, { ImGuiKey_RightArrow, kVK_RightArrow },
        { ImGuiKey_UpArrow, kVK_UpArrow }, { ImGuiKey_DownArrow, kVK_DownArrow },
    };
    const auto found = codes.find(key);
    return found == codes.end() ? UINT32_MAX : found->second;
}

OSStatus OnHotkey(EventHandlerCallRef, EventRef event, void*)
{
    EventHotKeyID id{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id) != noErr)
        return eventNotHandledErr;
    // Handled from the main loop, outside of AppKit's event dispatch.
    pendingHotkeys.emplace_back(int(id.id), GetEventKind(event) == kEventHotKeyPressed);
    glfwPostEmptyEvent();
    return noErr;
}

void Spawn(const char* program, const std::string& argument)
{
    const char* argv[] = { program, argument.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, program, nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0)
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}
} // namespace

bool Init(std::string&)
{
    captureQueue = dispatch_queue_create("me.unishade.capture", DISPATCH_QUEUE_SERIAL);
    const EventTypeSpec types[] = { { kEventClassKeyboard, kEventHotKeyPressed }, { kEventClassKeyboard, kEventHotKeyReleased } };
    InstallApplicationEventHandler(&OnHotkey, 2, types, nullptr, &hotkeyHandler);
    return true;
}

void Shutdown()
{
    StopCapture();
    for (const auto& [id, ref] : std::map<int, EventHotKeyRef>(hotkeys))
        UnregisterHotkey(id);
    if (hotkeyHandler)
        RemoveEventHandler(hotkeyHandler);
    hotkeyHandler = nullptr;
}

std::vector<Window> ListWindows()
{
    std::vector<Window> windows;
    const int self = getpid();
    for (NSDictionary* info in WindowList(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
    {
        const int pid = [info[(id)kCGWindowOwnerPID] intValue];
        Rect bounds;
        if ([info[(id)kCGWindowLayer] intValue] != 0 || pid == self || [info[(id)kCGWindowAlpha] doubleValue] <= 0 || !Bounds(info, bounds) ||
            bounds.width < 64 || bounds.height < 64)
            continue;
        // Window titles need the screen recording permission. The program's name stands in without it.
        NSString* name = info[(id)kCGWindowName];
        NSString* owner = info[(id)kCGWindowOwnerName];
        NSString* title = name.length ? name : owner;
        if (!title.length)
            continue;
        windows.push_back({ [info[(id)kCGWindowNumber] unsignedLongLongValue], title.UTF8String, pid });
    }
    std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
    return windows;
}

bool WindowExists(const Window& window)
{
    NSArray* list = WindowList(kCGWindowListOptionIncludingWindow, CGWindowID(window.id));
    return list.count > 0 && [list[0][(id)kCGWindowOwnerPID] intValue] == window.pid;
}

// The whole window, title bar included, since that is what ScreenCaptureKit copies of a single window.
bool WindowBounds(WindowId window, Rect& bounds)
{
    NSArray* list = WindowList(kCGWindowListOptionIncludingWindow, CGWindowID(window));
    return list.count > 0 && [list[0][(id)kCGWindowIsOnscreen] boolValue] && Bounds(list[0], bounds) && bounds.width > 0 && bounds.height > 0;
}

WindowId ForegroundWindow()
{
    const int pid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    // The list is in front-to-back order, so the first window of the program in front is its front window.
    for (NSDictionary* info in WindowList(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
        if ([info[(id)kCGWindowOwnerPID] intValue] == pid && [info[(id)kCGWindowLayer] intValue] == 0)
            return [info[(id)kCGWindowNumber] unsignedLongLongValue];
    return 0;
}

bool ProcessInFront(int pid)
{
    return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == pid;
}

void Activate(const Window& window)
{
    NSRunningApplication* target = [NSRunningApplication runningApplicationWithProcessIdentifier:window.pid];
    if (!target)
        return;
    if (@available(macOS 14.0, *))
    {
        [NSApp yieldActivationToApplication:target];
        [target activateWithOptions:0];
    }
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [target activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
    }
}

std::string ProcessExecutable(int pid)
{
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const int size = pid > 0 ? proc_pidpath(pid, path, sizeof(path)) : 0;
    return size > 0 ? std::string(path, size) : std::string();
}

std::string ProcessCommand(int pid)
{
    return ProcessArgument(pid);
}

std::vector<Process> ListProcesses()
{
    std::vector<pid_t> pids(4096);
    const int count = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    std::vector<Process> processes;
    for (int i = 0; i < count && i < int(pids.size()); ++i)
    {
        std::string executable = ProcessExecutable(pids[i]);
        if (executable.empty())
            continue;
        // Only Wine starts programs whose arguments name another executable, so the arguments are read only there.
        const std::string name = executable.substr(executable.find_last_of('/') + 1);
        std::string command = name.find("wine") != std::string::npos || name.find("preloader") != std::string::npos ? ProcessArgument(pids[i]) : "";
        processes.push_back({ pids[i], std::move(executable), std::move(command) });
    }
    return processes;
}

void SetupOverlayWindow(GLFWwindow* window)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    // On every Space, beside full-screen games too, and never in the window cycle.
    overlay.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
                                 NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    overlay.level = NSStatusWindowLevel;
    overlay.hasShadow = NO;
    overlay.hidesOnDeactivate = NO;
}

void ShowOverlay(GLFWwindow* window, bool visible)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    if (visible)
        [overlay orderFrontRegardless];
    else
        [overlay orderOut:nil];
}

void FocusOverlay(GLFWwindow* window)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
    [overlay makeKeyAndOrderFront:nil];
}

bool StartCapture(const Window& window, std::string& error)
{
    StopCapture();
    Rect bounds;
    if (!WindowBounds(window.id, bounds))
    {
        error = "The window is not visible.";
        return false;
    }
    const uint64_t generation = ++capture.generation;
    {
        std::lock_guard lock(capture.mutex);
        capture.error.clear();
        capture.ready = {};
        capture.back = {};
        capture.reconfiguring = false;
    }
    capture.running = true;
    const CGWindowID windowId = CGWindowID(window.id);

    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:NO
                                 completionHandler:^(SCShareableContent* content, NSError* contentError) {
                                   if (generation != capture.generation)
                                       return;
                                   if (!content)
                                   {
                                       FailCapture(generation, std::string("Could not list windows to capture: ") +
                                                                   (contentError ? contentError.localizedDescription.UTF8String : "unknown error"));
                                       return;
                                   }
                                   SCWindow* target = nil;
                                   for (SCWindow* candidate in content.windows)
                                       if (candidate.windowID == windowId)
                                           target = candidate;
                                   if (!target)
                                   {
                                       FailCapture(generation, "The game's window cannot be captured.");
                                       return;
                                   }
                                   SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
                                   double scale = NSScreen.mainScreen.backingScaleFactor;
                                   if (@available(macOS 14.0, *))
                                       scale = filter.pointPixelScale;
                                   SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
                                   config.width = size_t(std::lround(target.frame.size.width * scale));
                                   config.height = size_t(std::lround(target.frame.size.height * scale));
                                   config.pixelFormat = kCVPixelFormatType_32BGRA;
                                   config.showsCursor = NO;
                                   // As fast as the game draws, so the overlay adds no delay of its own.
                                   config.minimumFrameInterval = CMTimeMake(1, 240);
                                   config.queueDepth = 5;
                                   config.colorSpaceName = kCGColorSpaceSRGB;
                                   if (@available(macOS 14.0, *))
                                       config.ignoreShadowsSingleWindow = YES;
                                   {
                                       std::lock_guard lock(capture.mutex);
                                       capture.configWidth = uint32_t(config.width);
                                       capture.configHeight = uint32_t(config.height);
                                   }

                                   UnishadeStreamOutput* handler = [[UnishadeStreamOutput alloc] init];
                                   handler.generation = generation;
                                   SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:handler];
                                   NSError* addError = nil;
                                   if (![stream addStreamOutput:handler type:SCStreamOutputTypeScreen sampleHandlerQueue:captureQueue error:&addError])
                                   {
                                       FailCapture(generation, std::string("Could not capture the game: ") + addError.localizedDescription.UTF8String);
                                       return;
                                   }
                                   handler.stream = stream;
                                   dispatch_async(dispatch_get_main_queue(), ^{
                                     // Stopped or replaced before it started.
                                     if (generation != capture.generation)
                                     {
                                         [handler.stream stopCaptureWithCompletionHandler:nil];
                                         handler.stream = nil;
                                         return;
                                     }
                                     output = handler;
                                   });
                                   [stream startCaptureWithCompletionHandler:^(NSError* startError) {
                                     if (startError)
                                         FailCapture(generation, std::string("Could not capture the game: ") + startError.localizedDescription.UTF8String);
                                   }];
                                 }];
    return true;
}

void StopCapture()
{
    ++capture.generation;
    capture.running = false;
    if (output)
    {
        [output.stream stopCaptureWithCompletionHandler:nil];
        output.stream = nil;
        output = nil;
    }
    std::lock_guard lock(capture.mutex);
    capture.ready = {};
}

bool Capturing()
{
    return capture.running;
}

bool TakeFrame(Frame& frame)
{
    std::lock_guard lock(capture.mutex);
    if (capture.ready.serial <= frame.serial || capture.ready.pixels.empty())
        return false;
    std::swap(frame, capture.ready);
    return true;
}

std::string CaptureError()
{
    std::lock_guard lock(capture.mutex);
    return capture.error;
}

bool HasCapturePermission()
{
    return CGPreflightScreenCaptureAccess();
}

void RequestCapturePermission()
{
    // Asks once. After that, only System Settings can change it.
    if (!CGRequestScreenCaptureAccess())
        Open("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture");
}

void SetHotkeyCallback(HotkeyCallback callback)
{
    hotkeyCallback = std::move(callback);
}

bool RegisterHotkey(int id, const Hotkey& hotkey)
{
    UnregisterHotkey(id);
    const UInt32 code = KeyCode(hotkey.key);
    if (code == UINT32_MAX)
        return false;
    const UInt32 modifiers = ((hotkey.modifiers & kCtrl) ? controlKey : 0) | ((hotkey.modifiers & kAlt) ? optionKey : 0) |
                             ((hotkey.modifiers & kShift) ? shiftKey : 0) | ((hotkey.modifiers & kSuper) ? cmdKey : 0);
    EventHotKeyRef ref = nullptr;
    const EventHotKeyID hotkeyId{ 'UNSH', UInt32(id) };
    if (RegisterEventHotKey(code, modifiers, hotkeyId, GetApplicationEventTarget(), 0, &ref) != noErr || !ref)
        return false;
    hotkeys[id] = ref;
    return true;
}

void UnregisterHotkey(int id)
{
    const auto found = hotkeys.find(id);
    if (found == hotkeys.end())
        return;
    UnregisterEventHotKey(found->second);
    hotkeys.erase(found);
}

void PollHotkeys()
{
    std::vector<std::pair<int, bool>> events;
    events.swap(pendingHotkeys);
    for (const auto& [id, pressed] : events)
        if (hotkeyCallback && hotkeys.count(id))
            hotkeyCallback(id, pressed);
}

void Open(const std::string& target)
{
    Spawn("open", target);
}

std::string UiFont()
{
    for (const char* path : { "/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial.ttf" })
        if (access(path, R_OK) == 0)
            return path;
    return {};
}
} // namespace platform
