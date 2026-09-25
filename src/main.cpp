// RobloxShadeHost: redraws the Roblox window in a D3D11 swapchain of its own, so ReShade can be
// installed on this exe instead of Roblox. Roblox is only observed from outside, through window
// enumeration and Windows.Graphics.Capture. Nothing is opened, read or loaded into its process.

#include "addon.h"
#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "log.h"
#include "overlay.h"
#include "roblox_window.h"
#include "state.h"

#include <cstdio>

State g;

namespace
{
bool RegisterInputHotkey(const Hotkey& hotkey)
{
    if (RegisterHotKey(g.overlay, kEditModeHotkey, hotkey.modifiers, hotkey.key))
        return true;
    static bool reported = false;
    if (!reported)
        Log(LogLevel::Warning, L"%ls is in use by another program. Choose another shortcut in RobloxShadeHost Setup or in "
                               L"RobloxShadeHost.ini, then restart RobloxShadeHost.",
            g.inputHotkey.c_str());
    reported = true;
    return false;
}

// Holding a bare key such as Home all the time would break it in every other program, so the shortcut is
// only registered while Roblox or the host is in front.
void UpdateInputHotkey(const Hotkey& hotkey)
{
    const bool wanted = g.target && (g.editMode || GetForegroundWindow() == g.target);
    if (wanted == g.inputHotkeyRegistered)
        return;
    if (wanted)
        g.inputHotkeyRegistered = RegisterInputHotkey(hotkey);
    else
    {
        UnregisterHotKey(g.overlay, kEditModeHotkey);
        g.inputHotkeyRegistered = false;
    }
}

int Run()
{
    const auto hotkeys = LoadInputHotkeys();
    if (!GraphicsCaptureSession::IsSupported())
    {
        Log(LogLevel::Error, L"Windows Graphics Capture is not available, and RobloxShadeHost needs it to copy Roblox's picture. "
                             L"Update Windows and your graphics driver.");
        return 1;
    }

    CreateOverlayWindows();
    InitAddon();
    g.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateDevice();
    InitDepth();

    // Reports a taken shortcut now rather than on the first press in Roblox.
    if (RegisterInputHotkey(hotkeys.input))
        UnregisterHotKey(g.overlay, kEditModeHotkey);
    if (hotkeys.overlay.key && !RegisterHotKey(g.overlay, kOverlayToggleHotkey, hotkeys.overlay.modifiers, hotkeys.overlay.key))
        Log(LogLevel::Warning, L"%ls is in use by another program, so the overlay shortcut is off. Choose another shortcut in "
                               L"RobloxShadeHost Setup or in RobloxShadeHost.ini, then restart RobloxShadeHost.",
            g.overlayHotkey.c_str());

    Log(LogLevel::Info, L"Install ReShade on this exe (DirectX 10/11/12).");
    if (AddonRegistered())
        Log(LogLevel::Info, L"Press %ls in Roblox to open ReShade, and again to go back to Roblox.", g.inputHotkey.c_str());
    else
        Log(LogLevel::Info, L"Press %ls in Roblox to use ReShade's menu, and again to go back to Roblox.", g.inputHotkey.c_str());
    if (hotkeys.overlay.key)
        Log(LogLevel::Info, L"Press %ls to turn the overlay off and on.", g.overlayHotkey.c_str());
    Log(LogLevel::Info, L"Log file: %ls", LogPath().c_str());
    Log(LogLevel::Info, L"Waiting for Roblox...");

    ULONGLONG nextSearch = 0;
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                ShutdownDepth();
                ShutdownAddon();
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g.captureEnabled && g.target)
            StopCapture();

        if (g.target && !IsWindow(g.target))
        {
            StopCapture();
            Log(LogLevel::Info, L"Roblox closed. Waiting for Roblox...");
        }

        if (g.captureEnabled && !g.target && GetTickCount64() >= nextSearch)
        {
            nextSearch = GetTickCount64() + 500;
            if (HWND roblox = FindRobloxWindow())
            {
                try
                {
                    StartCapture(roblox);
                }
                catch (const winrt::hresult_error& e)
                {
                    Log(LogLevel::Error, L"Could not capture Roblox: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
                }
            }
        }

        UpdateOverlay();
        UpdateInputHotkey(hotkeys.input);

        if (g.target)
        {
            // Only the newest frame matters. Rendering every queued frame would add latency.
            while (auto frame = g.pool.TryGetNextFrame())
                g.latestFrame = frame;

            if (g.latestFrame)
            {
                SizeInt32 size = g.latestFrame.ContentSize();
                if ((size.Width != g.poolSize.Width || size.Height != g.poolSize.Height) && size.Width > 0 && size.Height > 0)
                {
                    g.latestFrame = nullptr;
                    g.poolSize = size;
                    g.pool.Recreate(g.captureDevice, kPixelFormat, 2, size);
                    Log(LogLevel::Info, L"Roblox resized to %dx%d", size.Width, size.Height);
                }
            }

            // Also re-presents on timeout, so the ReShade menu stays responsive if Roblox stops drawing.
            if (g.overlayVisible && g.latestFrame)
                PresentLatestFrame();
        }

        MsgWaitForMultipleObjects(1, &g.frameEvent, FALSE, g.overlayVisible ? 16 : 250, QS_ALLINPUT);
    }
}

// A console window the host opened itself would close with the error in it.
void KeepConsoleOpen()
{
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) != 1)
        return;
    std::puts("\nPress Enter to close.");
    std::getchar();
}
} // namespace

int main()
{
    std::puts(R"(  ____       _     _            ____  _               _      _   _           _
 |  _ \ ___ | |__ | | _____  __/ ___|| |__   __ _  __| | ___| | | | ___  ___| |_
 | |_) / _ \| '_ \| |/ _ \ \/ /\___ \| '_ \ / _` |/ _` |/ _ \ |_| |/ _ \/ __| __|
 |  _ < (_) | |_) | | (_) >  <  ___) | | | | (_| | (_| |  __/  _  | (_) \__ \ |_
 |_| \_\___/|_.__/|_|\___/_/\_\|____/|_| |_|\__,_|\__,_|\___|_| |_|\___/|___/\__|
)");
    std::printf("v%s\n\n", ROBLOX_SHADE_HOST_VERSION);
    InitLog();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int result = 1;
    try
    {
        result = Run();
    }
    catch (const winrt::hresult_error& e)
    {
        Log(LogLevel::Error, L"RobloxShadeHost stopped: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"RobloxShadeHost stopped: %hs", e.what());
    }
    if (result != 0)
        KeepConsoleOpen();
    return result;
}
