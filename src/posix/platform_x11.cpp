// Linux: X11, which also covers games that run through XWayland, such as Wine and Proton games under a Wayland
// desktop. Windows come from the window manager's _NET_CLIENT_LIST, pictures from XComposite, which keeps a
// window's picture even where other windows cover it, and shortcuts from passive key grabs on the root window.

#include "platform.h"
#include "gpu.h"
#include "log.h"

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/keysym.h>
#ifdef UNISHADE_HAVE_XRES
#include <X11/extensions/XRes.h>
#endif
#ifdef UNISHADE_HAVE_DRI3
#include <X11/Xlib-xcb.h>
#include <xcb/dri3.h>
#endif
#include <dlfcn.h>
#include <stb_image_write.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <thread>

extern char** environ;

namespace platform
{
namespace
{
Display* display = nullptr; // the main thread's own connection, apart from GLFW's
::Window root = 0;
bool haveXRes = false;

#ifdef UNISHADE_HAVE_XRES
// Loaded when present rather than linked, so the host also starts on systems without libXRes.
struct XResFunctions
{
    decltype(&XResQueryExtension) queryExtension = nullptr;
    decltype(&XResQueryVersion) queryVersion = nullptr;
    decltype(&XResQueryClientIds) queryClientIds = nullptr;
    decltype(&XResGetClientIdType) getClientIdType = nullptr;
    decltype(&XResGetClientPid) getClientPid = nullptr;
    decltype(&XResClientIdsDestroy) clientIdsDestroy = nullptr;

    bool Load()
    {
        void* library = dlopen("libXRes.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            return false;
        queryExtension = reinterpret_cast<decltype(queryExtension)>(dlsym(library, "XResQueryExtension"));
        queryVersion = reinterpret_cast<decltype(queryVersion)>(dlsym(library, "XResQueryVersion"));
        queryClientIds = reinterpret_cast<decltype(queryClientIds)>(dlsym(library, "XResQueryClientIds"));
        getClientIdType = reinterpret_cast<decltype(getClientIdType)>(dlsym(library, "XResGetClientIdType"));
        getClientPid = reinterpret_cast<decltype(getClientPid)>(dlsym(library, "XResGetClientPid"));
        clientIdsDestroy = reinterpret_cast<decltype(clientIdsDestroy)>(dlsym(library, "XResClientIdsDestroy"));
        return queryExtension && queryVersion && queryClientIds && getClientIdType && getClientPid && clientIdsDestroy;
    }
};
XResFunctions xres;
#endif

// Xlib's default error handler ends the process. Errors are expected here, since windows close at any time, so
// they are recorded and checked instead.
thread_local int lastError = 0;
int OnXError(Display*, XErrorEvent* event)
{
    lastError = event->error_code;
    return 0;
}

struct ErrorTrap
{
    Display* connection;
    explicit ErrorTrap(Display* d) : connection(d)
    {
        XSync(connection, False);
        lastError = 0;
    }
    bool Failed()
    {
        XSync(connection, False);
        return lastError != 0;
    }
};

Atom GetAtom(Display* d, const char* name)
{
    return XInternAtom(d, name, False);
}

// Reads a property of 32-bit items, such as window IDs or cardinals.
std::vector<unsigned long> Property32(Display* d, ::Window window, Atom property, Atom type)
{
    Atom actualType;
    int format;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    std::vector<unsigned long> result;
    if (XGetWindowProperty(d, window, property, 0, 4096, False, type, &actualType, &format, &count, &remaining, &data) == Success && data)
    {
        if (format == 32)
            result.assign(reinterpret_cast<unsigned long*>(data), reinterpret_cast<unsigned long*>(data) + count);
        XFree(data);
    }
    return result;
}

std::string Title(::Window window)
{
    Atom actualType;
    int format;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    std::string title;
    if (XGetWindowProperty(display, window, GetAtom(display, "_NET_WM_NAME"), 0, 1024, False, GetAtom(display, "UTF8_STRING"), &actualType,
                           &format, &count, &remaining, &data) == Success && data)
    {
        title.assign(reinterpret_cast<char*>(data), count);
        XFree(data);
    }
    if (title.empty())
    {
        char* name = nullptr;
        if (XFetchName(display, window, &name) && name)
        {
            title = name;
            XFree(name);
        }
    }
    return title;
}

// The process that owns a window. The X server knows it from the connection, which also works for sandboxed
// programs like Flatpak apps, whose own idea of their process ID differs from everyone else's.
int WindowPid(::Window window)
{
#ifdef UNISHADE_HAVE_XRES
    if (haveXRes)
    {
        XResClientIdSpec spec{ window, XRES_CLIENT_ID_PID_MASK };
        long count = 0;
        XResClientIdValue* values = nullptr;
        int pid = 0;
        if (xres.queryClientIds(display, 1, &spec, &count, &values) == Success)
        {
            for (long i = 0; i < count && !pid; ++i)
                if (xres.getClientIdType(&values[i]) == XRES_CLIENT_ID_PID)
                    pid = xres.getClientPid(&values[i]);
            xres.clientIdsDestroy(count, values);
        }
        if (pid > 0)
            return pid;
    }
#endif
    const auto pid = Property32(display, window, GetAtom(display, "_NET_WM_PID"), XA_CARDINAL);
    return pid.empty() ? 0 : static_cast<int>(pid[0]);
}

bool Hidden(::Window window)
{
    const Atom hidden = GetAtom(display, "_NET_WM_STATE_HIDDEN");
    for (unsigned long state : Property32(display, window, GetAtom(display, "_NET_WM_STATE"), XA_ATOM))
        if (state == hidden)
            return true;
    return false;
}

std::string ReadLink(const std::string& path)
{
    char buffer[4096];
    const ssize_t size = readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    return size > 0 ? std::string(buffer, size) : std::string();
}

// Capture

// The window's picture as the X server keeps it on the graphics card, from DRI3. Closes its file descriptors when
// the last frame using it is gone.
struct DmaBuffer
{
    std::vector<int> fds;
    std::vector<uint32_t> strides;
    std::vector<uint32_t> offsets;
    uint64_t modifier = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    ~DmaBuffer()
    {
        for (int fd : fds)
            close(fd);
    }
};

#ifdef UNISHADE_HAVE_DRI3
// DRI3 through XCB, loaded when present rather than linked, so the host starts where it is missing.
struct Dri3Functions
{
    decltype(&XGetXCBConnection) getConnection = nullptr;
    decltype(&xcb_get_extension_data) extensionData = nullptr;
    xcb_extension_t* extension = nullptr;
    decltype(&xcb_dri3_query_version) queryVersion = nullptr;
    decltype(&xcb_dri3_query_version_reply) queryVersionReply = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap) buffersFromPixmap = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_reply) buffersFromPixmapReply = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_strides) strides = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_offsets) offsets = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_buffers) buffers = nullptr;
    decltype(&xcb_dri3_open) open = nullptr;
    decltype(&xcb_dri3_open_reply) openReply = nullptr;
    decltype(&xcb_dri3_open_reply_fds) openReplyFds = nullptr;

    bool Load()
    {
        void* xlibXcb = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void* xcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void* dri3 = dlopen("libxcb-dri3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!xlibXcb || !xcb || !dri3)
            return false;
        getConnection = reinterpret_cast<decltype(getConnection)>(dlsym(xlibXcb, "XGetXCBConnection"));
        extensionData = reinterpret_cast<decltype(extensionData)>(dlsym(xcb, "xcb_get_extension_data"));
        extension = static_cast<xcb_extension_t*>(dlsym(dri3, "xcb_dri3_id"));
        queryVersion = reinterpret_cast<decltype(queryVersion)>(dlsym(dri3, "xcb_dri3_query_version"));
        queryVersionReply = reinterpret_cast<decltype(queryVersionReply)>(dlsym(dri3, "xcb_dri3_query_version_reply"));
        buffersFromPixmap = reinterpret_cast<decltype(buffersFromPixmap)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap"));
        buffersFromPixmapReply = reinterpret_cast<decltype(buffersFromPixmapReply)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_reply"));
        strides = reinterpret_cast<decltype(strides)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_strides"));
        offsets = reinterpret_cast<decltype(offsets)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_offsets"));
        buffers = reinterpret_cast<decltype(buffers)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_buffers"));
        open = reinterpret_cast<decltype(open)>(dlsym(dri3, "xcb_dri3_open"));
        openReply = reinterpret_cast<decltype(openReply)>(dlsym(dri3, "xcb_dri3_open_reply"));
        openReplyFds = reinterpret_cast<decltype(openReplyFds)>(dlsym(dri3, "xcb_dri3_open_reply_fds"));
        return getConnection && extensionData && extension && queryVersion && queryVersionReply && buffersFromPixmap && buffersFromPixmapReply &&
               strides && offsets && buffers && open && openReply && openReplyFds;
    }
};

// Null when DRI3 is missing, here or in the X server.
const Dri3Functions* Dri3(Display* d)
{
    static Dri3Functions dri3;
    static const bool loaded = dri3.Load();
    if (!loaded)
        return nullptr;
    const xcb_query_extension_reply_t* present = dri3.extensionData(dri3.getConnection(d), dri3.extension);
    return present && present->present ? &dri3 : nullptr;
}

// The pixmap's buffers, with DRI3 1.2's modifiers. Empty where the X server cannot share them, such as without
// a GPU or with drivers that do not support DRI3.
std::shared_ptr<DmaBuffer> BuffersFromPixmap(Display* d, Pixmap pixmap)
{
    const Dri3Functions* functions = Dri3(d);
    if (!functions)
        return nullptr;
    const Dri3Functions& dri3 = *functions;
    xcb_connection_t* connection = dri3.getConnection(d);
    xcb_dri3_query_version_reply_t* version = dri3.queryVersionReply(connection, dri3.queryVersion(connection, 1, 2), nullptr);
    const bool modifiers = version && (version->major_version > 1 || version->minor_version >= 2);
    free(version);
    if (!modifiers)
        return nullptr;
    xcb_dri3_buffers_from_pixmap_reply_t* reply = dri3.buffersFromPixmapReply(connection, dri3.buffersFromPixmap(connection, pixmap), nullptr);
    if (!reply)
        return nullptr;
    auto buffer = std::make_shared<DmaBuffer>();
    const int32_t* fds = dri3.buffers(reply);
    for (int i = 0; i < reply->nfd; ++i)
    {
        buffer->fds.push_back(fds[i]);
        buffer->strides.push_back(dri3.strides(reply)[i]);
        buffer->offsets.push_back(dri3.offsets(reply)[i]);
    }
    buffer->modifier = reply->modifier;
    buffer->width = reply->width;
    buffer->height = reply->height;
    const bool usable = reply->nfd > 0 && reply->bpp == 32 && (reply->depth == 24 || reply->depth == 32);
    free(reply);
    return usable ? buffer : nullptr;
}
#else
std::shared_ptr<DmaBuffer> BuffersFromPixmap(Display*, Pixmap)
{
    return nullptr;
}
#endif

// Frees the dma-buf imported into Vulkan. Defined with TakeFrame.
void ReleaseImported();

struct Capture
{
    std::thread thread;
    std::atomic<bool> stop = false;
    std::atomic<bool> running = false;
    std::atomic<bool> onGpu = false; // frames go to the main thread as dma-bufs instead of pixels
    std::atomic<bool> reset = false; // the capture thread names the pixmap again, to change how it copies
    std::mutex mutex;
    Frame ready;
    std::string error;
};
Capture capture;

void CaptureThread(::Window target, int refresh)
{
    Display* d = XOpenDisplay(nullptr);
    if (!d)
    {
        std::lock_guard lock(capture.mutex);
        capture.error = "Could not connect to the X server to copy the game's picture.";
        capture.running = false;
        return;
    }
    const bool shm = XShmQueryExtension(d);
    {
        ErrorTrap trap(d);
        XCompositeRedirectWindow(d, target, CompositeRedirectAutomatic);
        trap.Failed();
    }

    // Copies as often as the display refreshes. XComposite has no event for a new frame.
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / refresh);

    Pixmap pixmap = 0;
    std::shared_ptr<DmaBuffer> buffer;
    XImage* image = nullptr;
    XShmSegmentInfo segment{};
    int width = 0, height = 0;
    Frame back;
    uint64_t serial = 0;
    std::string failure;

    const auto release = [&] {
        if (image)
        {
            if (shm)
            {
                XShmDetach(d, &segment);
                shmdt(segment.shmaddr);
                segment = {};
            }
            XDestroyImage(image);
            image = nullptr;
        }
        if (pixmap)
            XFreePixmap(d, pixmap);
        pixmap = 0;
        buffer.reset();
    };

    auto next = std::chrono::steady_clock::now();
    while (!capture.stop)
    {
        next += interval;
        XWindowAttributes attributes{};
        {
            ErrorTrap trap(d);
            const Status ok = XGetWindowAttributes(d, target, &attributes);
            if (trap.Failed() || !ok)
            {
                failure = "The game's window closed.";
                break;
            }
        }
        if (attributes.map_state != IsViewable || attributes.width <= 0 || attributes.height <= 0)
        {
            release();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next = std::chrono::steady_clock::now();
            continue;
        }
        if (attributes.width != width || attributes.height != height || !pixmap || capture.reset.exchange(false))
        {
            release();
            width = attributes.width;
            height = attributes.height;
            ErrorTrap trap(d);
            pixmap = XCompositeNameWindowPixmap(d, target);
            if (trap.Failed())
            {
                pixmap = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (capture.onGpu)
            {
                buffer = BuffersFromPixmap(d, pixmap);
                if (!buffer || buffer->width != unsigned(width) || buffer->height != unsigned(height))
                {
                    buffer.reset();
                    capture.onGpu = false;
                    Log(LogLevel::Info, "The X server cannot share the game's picture on the graphics card. Copying it instead.");
                }
            }
            if (shm && !buffer)
            {
                image = XShmCreateImage(d, attributes.visual, attributes.depth, ZPixmap, nullptr, &segment, width, height);
                if (image)
                {
                    segment.shmid = shmget(IPC_PRIVATE, size_t(image->bytes_per_line) * image->height, IPC_CREAT | 0600);
                    segment.shmaddr = image->data = static_cast<char*>(shmat(segment.shmid, nullptr, 0));
                    segment.readOnly = False;
                    XShmAttach(d, &segment);
                    // Removed now, so it goes away with the process even if it crashes.
                    shmctl(segment.shmid, IPC_RMID, nullptr);
                }
            }
            if (image && (image->bits_per_pixel != 32 || image->red_mask != 0xFF0000 || image->blue_mask != 0xFF))
            {
                failure = "The game's window uses a pixel format Unishade cannot read.";
                break;
            }
        }

        if (buffer)
        {
            // The X server keeps drawing into the same buffer, so each frame only says it is there to use.
            back.pixels.clear();
            back.width = width;
            back.height = height;
            back.hold = buffer;
            {
                std::lock_guard lock(capture.mutex);
                back.serial = ++serial;
                std::swap(back, capture.ready);
            }
            back.hold.reset();
            glfwPostEmptyEvent();
            std::this_thread::sleep_until(next);
            if (std::chrono::steady_clock::now() > next + interval)
                next = std::chrono::steady_clock::now();
            continue;
        }

        XImage* got = nullptr;
        {
            ErrorTrap trap(d);
            if (shm && image)
                got = XShmGetImage(d, pixmap, image, 0, 0, AllPlanes) ? image : nullptr;
            else
                got = XGetImage(d, pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
            if (trap.Failed())
                got = nullptr;
        }
        if (!got)
        {
            // The window changed between the size check and the copy. The next round names its pixmap again.
            release();
            continue;
        }
        if (got->bits_per_pixel != 32)
        {
            failure = "The game's window uses a pixel format Unishade cannot read.";
            if (got != image)
                XDestroyImage(got);
            break;
        }

        back.width = width;
        back.height = height;
        back.hold.reset();
        back.pixels.resize(size_t(width) * height);
        for (int y = 0; y < height; ++y)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(got->data + size_t(y) * got->bytes_per_line);
            uint32_t* out = back.pixels.data() + size_t(y) * width;
            // Windows without an alpha channel leave it undefined.
            for (int x = 0; x < width; ++x)
                out[x] = row[x] | 0xFF000000u;
        }
        if (got != image)
            XDestroyImage(got);
        {
            std::lock_guard lock(capture.mutex);
            back.serial = ++serial;
            std::swap(back, capture.ready);
        }
        glfwPostEmptyEvent();
        std::this_thread::sleep_until(next);
        if (std::chrono::steady_clock::now() > next + interval)
            next = std::chrono::steady_clock::now();
    }

    release();
    {
        ErrorTrap trap(d);
        XCompositeUnredirectWindow(d, target, CompositeRedirectAutomatic);
        trap.Failed();
    }
    XCloseDisplay(d);
    if (!failure.empty())
    {
        std::lock_guard lock(capture.mutex);
        capture.error = failure;
    }
    capture.running = false;
    glfwPostEmptyEvent();
}

// Hotkeys

struct Grab
{
    KeyCode code;
    unsigned modifiers;
};
std::map<int, Grab> grabs;
std::set<int> held;
HotkeyCallback hotkeyCallback;
std::thread waker;
std::atomic<bool> wakerStop = false;

KeySym Keysym(ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return XK_a + (key - ImGuiKey_A);
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return XK_0 + (key - ImGuiKey_0);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return XK_F1 + (key - ImGuiKey_F1);
    // The number pad's digits are one key with Num Lock on or off, so the shortcut works either way.
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return XK_KP_0 + (key - ImGuiKey_Keypad0);
    switch (key)
    {
    case ImGuiKey_Home: return XK_Home;
    case ImGuiKey_End: return XK_End;
    case ImGuiKey_Insert: return XK_Insert;
    case ImGuiKey_Delete: return XK_Delete;
    case ImGuiKey_PageUp: return XK_Prior;
    case ImGuiKey_PageDown: return XK_Next;
    case ImGuiKey_Pause: return XK_Pause;
    case ImGuiKey_ScrollLock: return XK_Scroll_Lock;
    case ImGuiKey_Space: return XK_space;
    case ImGuiKey_Tab: return XK_Tab;
    case ImGuiKey_Escape: return XK_Escape;
    case ImGuiKey_LeftArrow: return XK_Left;
    case ImGuiKey_RightArrow: return XK_Right;
    case ImGuiKey_UpArrow: return XK_Up;
    case ImGuiKey_DownArrow: return XK_Down;
    case ImGuiKey_KeypadMultiply: return XK_KP_Multiply;
    case ImGuiKey_KeypadAdd: return XK_KP_Add;
    case ImGuiKey_KeypadSubtract: return XK_KP_Subtract;
    case ImGuiKey_KeypadDecimal: return XK_KP_Decimal;
    case ImGuiKey_KeypadDivide: return XK_KP_Divide;
    default: return NoSymbol;
    }
}

unsigned XModifiers(unsigned modifiers)
{
    return ((modifiers & kCtrl) ? ControlMask : 0) | ((modifiers & kAlt) ? Mod1Mask : 0) | ((modifiers & kShift) ? ShiftMask : 0) |
           ((modifiers & kSuper) ? Mod4Mask : 0);
}

// Num Lock and Caps Lock are modifiers to X11, so each shortcut is grabbed with and without them.
constexpr unsigned kLockVariants[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
constexpr unsigned kRelevantModifiers = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;
} // namespace

bool Init(std::string& error)
{
    XInitThreads();
    XSetErrorHandler(OnXError);
    display = XOpenDisplay(nullptr);
    if (!display)
    {
        error = "Unishade needs an X11 or XWayland display.";
        return false;
    }
    root = DefaultRootWindow(display);
    int event, errorBase, major = 0, minor = 2;
    if (!XCompositeQueryExtension(display, &event, &errorBase) || !XCompositeQueryVersion(display, &major, &minor) || (major == 0 && minor < 2))
    {
        error = "The X server has no Composite extension, which Unishade needs to copy the game's picture.";
        return false;
    }
#ifdef UNISHADE_HAVE_XRES
    int xresMajor = 0, xresMinor = 0;
    haveXRes = xres.Load() && xres.queryExtension(display, &event, &errorBase) && xres.queryVersion(display, &xresMajor, &xresMinor) &&
               (xresMajor > 1 || (xresMajor == 1 && xresMinor >= 2));
#endif
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(display, True, &detectable);

    // GLFW only wakes for its own connection, so shortcuts on this one wake the loop from here.
    waker = std::thread([] {
        pollfd fd{ ConnectionNumber(display), POLLIN, 0 };
        while (!wakerStop)
            if (poll(&fd, 1, 200) > 0 && (fd.revents & POLLIN))
            {
                glfwPostEmptyEvent();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
    });

    if (getenv("WAYLAND_DISPLAY"))
        Log(LogLevel::Info, "Running through XWayland. Games that draw to Wayland directly cannot be captured.");
    return true;
}

void Shutdown()
{
    StopCapture();
    wakerStop = true;
    if (waker.joinable())
        waker.join();
    for (const auto& [id, grab] : std::map<int, Grab>(grabs))
        UnregisterHotkey(id);
    if (display)
        XCloseDisplay(display);
    display = nullptr;
}

std::vector<Window> ListWindows()
{
    std::vector<Window> windows;
    const Atom typeAtom = GetAtom(display, "_NET_WM_WINDOW_TYPE");
    std::set<Atom> skippedTypes;
    for (const char* name : { "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
                              "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_TOOLTIP" })
        skippedTypes.insert(GetAtom(display, name));

    std::vector<unsigned long> clients = Property32(display, root, GetAtom(display, "_NET_CLIENT_LIST"), XA_WINDOW);
    if (clients.empty())
    {
        // Without a window manager that lists windows, the root window's children are the top-level windows.
        ::Window parent, rootReturn, *children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(display, root, &rootReturn, &parent, &children, &count) && children)
        {
            clients.assign(children, children + count);
            XFree(children);
        }
    }

    const int self = getpid();
    ErrorTrap trap(display);
    for (unsigned long client : clients)
    {
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, client, &attributes) || attributes.map_state != IsViewable || attributes.override_redirect ||
            attributes.width < 64 || attributes.height < 64)
            continue;
        bool skip = false;
        for (unsigned long type : Property32(display, client, typeAtom, XA_ATOM))
            skip |= skippedTypes.count(type) != 0;
        if (skip || Hidden(client))
            continue;
        Window window{ client, Title(client), WindowPid(client) };
        if (window.title.empty() || window.pid == self)
            continue;
        windows.push_back(std::move(window));
    }
    trap.Failed();
    std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
    return windows;
}

bool WindowExists(const Window& window)
{
    ErrorTrap trap(display);
    XWindowAttributes attributes{};
    const bool exists = XGetWindowAttributes(display, window.id, &attributes) && !trap.Failed();
    return exists && WindowPid(window.id) == window.pid;
}

bool WindowBounds(WindowId window, Rect& bounds)
{
    ErrorTrap trap(display);
    XWindowAttributes attributes{};
    int x = 0, y = 0;
    ::Window child;
    if (!XGetWindowAttributes(display, window, &attributes) || attributes.map_state != IsViewable ||
        !XTranslateCoordinates(display, window, root, 0, 0, &x, &y, &child) || trap.Failed() || Hidden(window))
        return false;
    bounds = { x, y, attributes.width, attributes.height };
    return true;
}

WindowId ForegroundWindow()
{
    const auto active = Property32(display, root, GetAtom(display, "_NET_ACTIVE_WINDOW"), XA_WINDOW);
    if (!active.empty())
        return active[0];
    // Without a window manager that tracks it, the window with input focus, up to its top-level window.
    ::Window focus = 0;
    int revert;
    XGetInputFocus(display, &focus, &revert);
    ErrorTrap trap(display);
    while (focus && focus != root && focus != PointerRoot)
    {
        ::Window parent = 0, rootReturn, *children = nullptr;
        unsigned int count = 0;
        if (!XQueryTree(display, focus, &rootReturn, &parent, &children, &count))
            break;
        if (children)
            XFree(children);
        if (parent == root)
            return trap.Failed() ? 0 : focus;
        focus = parent;
    }
    return 0;
}

bool ProcessInFront(int pid)
{
    const WindowId window = ForegroundWindow();
    if (!window)
        return false;
    ErrorTrap trap(display);
    const int owner = WindowPid(window);
    return !trap.Failed() && owner == pid;
}

void Activate(const Window& window)
{
    // Source 2 says a pager asks, which window managers follow without their focus stealing prevention.
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window.id;
    event.xclient.message_type = GetAtom(display, "_NET_ACTIVE_WINDOW");
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

std::string ProcessExecutable(int pid)
{
    return pid > 0 ? ReadLink("/proc/" + std::to_string(pid) + "/exe") : std::string();
}

std::string ProcessCommand(int pid)
{
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string first;
    std::getline(input, first, '\0');
    return first;
}

std::vector<Process> ListProcesses()
{
    std::vector<Process> processes;
    DIR* proc = opendir("/proc");
    if (!proc)
        return processes;
    while (dirent* entry = readdir(proc))
    {
        const int pid = atoi(entry->d_name);
        if (pid <= 0)
            continue;
        Process process{ pid, ProcessExecutable(pid), ProcessCommand(pid) };
        if (!process.executable.empty() || !process.command.empty())
            processes.push_back(std::move(process));
    }
    closedir(proc);
    return processes;
}

void SetupOverlayWindow(GLFWwindow* window)
{
    Display* d = glfwGetX11Display();
    const ::Window x = glfwGetX11Window(window);
    const Atom states[] = { GetAtom(d, "_NET_WM_STATE_SKIP_TASKBAR"), GetAtom(d, "_NET_WM_STATE_SKIP_PAGER") };
    XChangeProperty(d, x, GetAtom(d, "_NET_WM_STATE"), XA_ATOM, 32, PropModeAppend, reinterpret_cast<const unsigned char*>(states), 2);
    XClassHint hint{ const_cast<char*>("unishade-overlay"), const_cast<char*>("Unishade") };
    XSetClassHint(d, x, &hint);
    XFlush(d);
}

void ShowOverlay(GLFWwindow* window, bool visible)
{
    if (!visible)
    {
        glfwHideWindow(window);
        return;
    }
    // A user time of zero asks the window manager not to focus the window when it appears.
    Display* d = glfwGetX11Display();
    const unsigned long zero = 0;
    XChangeProperty(d, glfwGetX11Window(window), GetAtom(d, "_NET_WM_USER_TIME"), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&zero), 1);
    glfwShowWindow(window);
    // Window managers can drop the above state while a window is hidden.
    glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_TRUE);
}

void FocusOverlay(GLFWwindow* window)
{
    Display* d = glfwGetX11Display();
    const ::Window x = glfwGetX11Window(window);
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = x;
    event.xclient.message_type = GetAtom(d, "_NET_ACTIVE_WINDOW");
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    // Without a window manager nobody answers that, so focus is set directly too.
    ErrorTrap trap(d);
    XSetInputFocus(d, x, RevertToParent, CurrentTime);
    trap.Failed();
}

bool StartCapture(const Window& window, std::string& error)
{
    StopCapture();
    {
        std::lock_guard lock(capture.mutex);
        capture.error.clear();
        capture.ready = {};
    }
    Rect bounds;
    if (!WindowBounds(window.id, bounds))
    {
        error = "The window is not visible.";
        return false;
    }
    int refresh = 60;
    if (const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor()); mode && mode->refreshRate > 0)
        refresh = std::clamp(mode->refreshRate, 30, 360);
    capture.stop = false;
    capture.running = true;
    capture.onGpu = gpu.dmaBuf;
    capture.reset = false;
    capture.thread = std::thread(CaptureThread, static_cast<::Window>(window.id), refresh);
    return true;
}

void StopCapture()
{
    capture.stop = true;
    if (capture.thread.joinable())
        capture.thread.join();
    capture.running = false;
    // The host waited for the graphics card before stopping.
    std::lock_guard lock(capture.mutex);
    capture.ready = {};
    ReleaseImported();
}

bool Capturing()
{
    return capture.running;
}

namespace
{
// The dma-buf imported into Vulkan, for as long as the X server uses the same buffer for the window.
struct Imported
{
    std::shared_ptr<void> buffer;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
Imported imported;

// The caller makes sure the graphics card no longer uses it.
void ReleaseImported()
{
    if (imported.image)
        vkDestroyImage(gpu.device, imported.image, nullptr);
    if (imported.memory)
        vkFreeMemory(gpu.device, imported.memory, nullptr);
    imported = {};
}

// Whether the graphics card can read the X server's buffer as it is laid out: its modifier with as many planes, for
// copying from, imported from a dma-buf at its size.
bool CanImport(const DmaBuffer& buffer, VkFormat format)
{
    VkDrmFormatModifierPropertiesListEXT list{ VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 properties{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
    properties.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(gpu.physicalDevice, format, &properties);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(gpu.physicalDevice, format, &properties);
    modifiers.resize(list.drmFormatModifierCount);
    const auto modifier = std::find_if(modifiers.begin(), modifiers.end(),
                                       [&](const VkDrmFormatModifierPropertiesEXT& entry) { return entry.drmFormatModifier == buffer.modifier; });
    if (modifier == modifiers.end() || modifier->drmFormatModifierPlaneCount != buffer.fds.size() ||
        !(modifier->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        return false;

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT };
    modifierInfo.drmFormatModifier = buffer.modifier;
    modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
    externalInfo.pNext = &modifierInfo;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
    info.pNext = &externalInfo;
    info.format = format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties external{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 result{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    result.pNext = &external;
    return vkGetPhysicalDeviceImageFormatProperties2(gpu.physicalDevice, &info, &result) == VK_SUCCESS &&
           (external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           result.imageFormatProperties.maxExtent.width >= buffer.width && result.imageFormatProperties.maxExtent.height >= buffer.height;
}

VkImage Import(const std::shared_ptr<void>& held)
{
    if (imported.buffer == held)
        return imported.image;
    vkDeviceWaitIdle(gpu.device);
    ReleaseImported();
    const DmaBuffer& buffer = *static_cast<const DmaBuffer*>(held.get());
    // XRGB8888 and ARGB8888, the formats of 24 and 32-bit windows.
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    if (buffer.fds.size() > 4 || !CanImport(buffer, format))
    {
        Log(LogLevel::Info, "The graphics card cannot read the X server's layout of the game's picture (modifier 0x%llx).",
            static_cast<unsigned long long>(buffer.modifier));
        return VK_NULL_HANDLE;
    }

    // Every plane must be in one buffer object, which is how drivers lay out RGB images.
    struct stat first{}, other{};
    if (fstat(buffer.fds[0], &first) != 0)
        return VK_NULL_HANDLE;
    for (int fd : buffer.fds)
        if (fstat(fd, &other) != 0 || other.st_ino != first.st_ino || other.st_dev != first.st_dev)
            return VK_NULL_HANDLE;

    std::vector<VkSubresourceLayout> planes;
    for (size_t i = 0; i < buffer.fds.size(); ++i)
        planes.push_back({ buffer.offsets[i], 0, buffer.strides[i], 0, 0 });
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
    modifier.drmFormatModifier = buffer.modifier;
    modifier.drmFormatModifierPlaneCount = uint32_t(planes.size());
    modifier.pPlaneLayouts = planes.data();
    VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.pNext = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { buffer.width, buffer.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Imported result;
    if (vkCreateImage(gpu.device, &info, nullptr, &result.image) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    const auto getFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(gpu.device, "vkGetMemoryFdPropertiesKHR"));
    VkMemoryFdPropertiesKHR fdProperties{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(gpu.device, result.image, &requirements);
    // Importing hands the file descriptor to Vulkan, so it gets its own copy.
    const int fd = dup(buffer.fds[0]);
    uint32_t type = UINT32_MAX;
    if (fd >= 0 && getFdProperties &&
        getFdProperties(gpu.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProperties) == VK_SUCCESS)
        type = gpu.FindMemoryType(requirements.memoryTypeBits & fdProperties.memoryTypeBits, 0);
    VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = result.image;
    VkImportMemoryFdInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = fd;
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.pNext = &import;
    // A dedicated allocation is the image's size, which the dma-buf must hold. Where the kernel cannot tell the
    // dma-buf's size, the driver checks it.
    const off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || (size >= 0 && VkDeviceSize(size) < requirements.size) ||
        vkAllocateMemory(gpu.device, &allocation, nullptr, &result.memory) != VK_SUCCESS)
    {
        if (fd >= 0)
            close(fd);
        vkDestroyImage(gpu.device, result.image, nullptr);
        return VK_NULL_HANDLE;
    }
    if (vkBindImageMemory(gpu.device, result.image, result.memory, 0) != VK_SUCCESS)
    {
        vkDestroyImage(gpu.device, result.image, nullptr);
        vkFreeMemory(gpu.device, result.memory, nullptr);
        return VK_NULL_HANDLE;
    }

    // Takes the image from the X server once to move it into the general layout, keeping its contents, then
    // hands it back. From here on each use takes and returns it in that layout.
    VkCommandBuffer commands = gpu.BeginCommands();
    const uint32_t outside = gpu.foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = outside;
    barrier.dstQueueFamilyIndex = gpu.queueFamily;
    barrier.image = result.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = gpu.queueFamily;
    barrier.dstQueueFamilyIndex = outside;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    gpu.SubmitAndWait(commands);

    result.buffer = held;
    imported = result;
    return imported.image;
}
} // namespace

bool TakeFrame(Frame& frame)
{
    {
        std::lock_guard lock(capture.mutex);
        if (capture.ready.serial <= frame.serial || (capture.ready.pixels.empty() && !capture.ready.hold))
            return false;
        std::swap(frame, capture.ready);
    }
    frame.image = VK_NULL_HANDLE;
    frame.foreign = false;
    if (frame.hold)
    {
        frame.image = Import(frame.hold);
        frame.foreign = true;
        if (!frame.image)
        {
            // Copies through memory from the next frame on, which the capture thread notices when it names the
            // window's pixmap again.
            Log(LogLevel::Warning, "Could not use the game's picture on the graphics card. Copying it instead.");
            capture.onGpu = false;
            capture.reset = true;
            return false;
        }
    }
    return true;
}

std::string CaptureError()
{
    std::lock_guard lock(capture.mutex);
    return capture.error;
}

bool HasCapturePermission()
{
    return true;
}

void RequestCapturePermission()
{
}

void SetHotkeyCallback(HotkeyCallback callback)
{
    hotkeyCallback = std::move(callback);
}

bool RegisterHotkey(int id, const Hotkey& hotkey)
{
    UnregisterHotkey(id);
    const KeySym sym = Keysym(hotkey.key);
    const KeyCode code = sym == NoSymbol ? 0 : XKeysymToKeycode(display, sym);
    if (!code)
        return false;
    const unsigned modifiers = XModifiers(hotkey.modifiers);
    ErrorTrap trap(display);
    for (unsigned variant : kLockVariants)
        XGrabKey(display, code, modifiers | variant, root, False, GrabModeAsync, GrabModeAsync);
    if (trap.Failed())
    {
        for (unsigned variant : kLockVariants)
            XUngrabKey(display, code, modifiers | variant, root);
        XSync(display, False);
        return false;
    }
    grabs[id] = { code, modifiers };
    return true;
}

void UnregisterHotkey(int id)
{
    const auto found = grabs.find(id);
    if (found == grabs.end())
        return;
    for (unsigned variant : kLockVariants)
        XUngrabKey(display, found->second.code, found->second.modifiers | variant, root);
    XFlush(display);
    grabs.erase(found);
    held.erase(id);
}

void PollHotkeys()
{
    while (XPending(display))
    {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type != KeyPress && event.type != KeyRelease)
            continue;
        const bool pressed = event.type == KeyPress;
        for (const auto& [id, grab] : grabs)
        {
            if (grab.code != event.xkey.keycode)
                continue;
            // A key let go ends its shortcut even when the modifiers were let go first.
            if (pressed ? (event.xkey.state & kRelevantModifiers) != grab.modifiers : !held.count(id))
                continue;
            if (pressed)
            {
                if (!held.insert(id).second)
                    continue; // held down, repeating
            }
            else
                held.erase(id);
            if (hotkeyCallback)
                hotkeyCallback(id, pressed);
        }
    }
}

bool SaveWindowIcon(const Window& window, const std::string& path)
{
    // _NET_WM_ICON holds each size as its width, its height and then ARGB pixels, one per 32-bit item.
    ErrorTrap trap(display);
    Atom actualType;
    int format = 0;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window.id, GetAtom(display, "_NET_WM_ICON"), 0, 1 << 22, False, XA_CARDINAL, &actualType, &format, &count,
                           &remaining, &data) != Success || !data)
    {
        trap.Failed();
        return false;
    }
    const unsigned long* items = reinterpret_cast<const unsigned long*>(data);
    size_t largest = 0, largestSize = 0;
    for (size_t i = 0; format == 32 && i + 2 <= count;)
    {
        const size_t size = items[i] * items[i + 1];
        if (!size || size > count - i - 2)
            break;
        if (size > largestSize)
        {
            largest = i;
            largestSize = size;
        }
        i += 2 + size;
    }
    bool saved = false;
    if (largestSize)
    {
        const int width = static_cast<int>(items[largest]), height = static_cast<int>(items[largest + 1]);
        std::vector<uint8_t> rgba(largestSize * 4);
        for (size_t p = 0; p < largestSize; ++p)
        {
            const unsigned long argb = items[largest + 2 + p];
            rgba[p * 4] = (argb >> 16) & 0xFF;
            rgba[p * 4 + 1] = (argb >> 8) & 0xFF;
            rgba[p * 4 + 2] = argb & 0xFF;
            rgba[p * 4 + 3] = (argb >> 24) & 0xFF;
        }
        saved = stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
    }
    XFree(data);
    return !trap.Failed() && saved;
}

void Open(const std::string& target)
{
    const char* argv[] = { "xdg-open", target.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0)
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}

std::string UiFont()
{
    if (FILE* pipe = popen("fc-match -f '%{file}' 'sans-serif:style=Regular' 2>/dev/null", "r"))
    {
        char buffer[1024]{};
        const size_t size = fread(buffer, 1, sizeof(buffer) - 1, pipe);
        pclose(pipe);
        const std::string path(buffer, size);
        const std::string extension = path.size() > 4 ? path.substr(path.size() - 4) : "";
        if ((extension == ".ttf" || extension == ".otf") && access(path.c_str(), R_OK) == 0)
            return path;
    }
    for (const char* path : { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                              "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                              "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" })
        if (access(path, R_OK) == 0)
            return path;
    return {};
}

bool DisplayDrmDevice([[maybe_unused]] int64_t& deviceMajor, [[maybe_unused]] int64_t& deviceMinor)
{
#ifdef UNISHADE_HAVE_DRI3
    const Dri3Functions* dri3 = display ? Dri3(display) : nullptr;
    if (!dri3)
        return false;
    // The device the X server hands its clients for drawing, the same one its own buffers live on.
    xcb_connection_t* connection = dri3->getConnection(display);
    xcb_dri3_open_reply_t* reply = dri3->openReply(connection, dri3->open(connection, root, 0), nullptr);
    if (!reply)
        return false;
    const int fd = reply->nfd == 1 ? dri3->openReplyFds(connection, reply)[0] : -1;
    free(reply);
    struct stat device{};
    const bool found = fd >= 0 && fstat(fd, &device) == 0 && S_ISCHR(device.st_mode);
    if (fd >= 0)
        close(fd);
    if (!found)
        return false;
    deviceMajor = major(device.st_rdev);
    deviceMinor = minor(device.st_rdev);
    return true;
#else
    return false;
#endif
}
} // namespace platform
