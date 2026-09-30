// Linux: X11, which also covers games that run through XWayland, such as Wine and Proton games under a Wayland
// desktop. Windows come from the window manager's _NET_CLIENT_LIST, pictures from XComposite, which keeps a
// window's picture even where other windows cover it, and shortcuts from passive key grabs on the root window.

#include "platform.h"
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
#include <dlfcn.h>
#endif

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

struct Capture
{
    std::thread thread;
    std::atomic<bool> stop = false;
    std::atomic<bool> running = false;
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
        if (attributes.width != width || attributes.height != height || !pixmap)
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
            if (shm)
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
    capture.thread = std::thread(CaptureThread, static_cast<::Window>(window.id), refresh);
    return true;
}

void StopCapture()
{
    capture.stop = true;
    if (capture.thread.joinable())
        capture.thread.join();
    capture.running = false;
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
} // namespace platform
