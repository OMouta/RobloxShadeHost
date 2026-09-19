// RobloxShadeHost: redraws the Roblox window in a D3D11 swapchain of its own, so ReShade can be
// installed on this exe instead of Roblox. Roblox is only observed from outside, through window
// enumeration and Windows.Graphics.Capture. Nothing is opened, read or loaded into its process.

#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "overlay.h"
#include "roblox_window.h"
#include "state.h"

#include <cstdio>

State g;

namespace
{
int Run()
{
    const auto hotkeys = LoadInputHotkeys();
    if (!GraphicsCaptureSession::IsSupported())
    {
        std::puts("Windows Graphics Capture is not supported on this system.");
        return 1;
    }

    CreateOverlayWindows();

    if (!RegisterHotKey(g.overlay, kEditModeHotkey, hotkeys.input.modifiers, hotkeys.input.key))
    {
        const std::wstring error = L"Could not register " + g.inputHotkey + L". It may be reserved by Windows or in use by another program. "
                                   L"Choose another ToggleKey in RobloxShadeHost.ini and restart.";
        MessageBoxW(nullptr, error.c_str(), L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (hotkeys.overlay.key && !RegisterHotKey(g.overlay, kOverlayToggleHotkey, hotkeys.overlay.modifiers, hotkeys.overlay.key))
    {
        const std::wstring error = L"Could not register " + g.overlayHotkey + L". It may be reserved by Windows or in use by another program. "
                                   L"Choose another OverlayToggleKey in RobloxShadeHost.ini and restart.";
        MessageBoxW(nullptr, error.c_str(), L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        return 1;
    }

    g.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateDevice();
    InitDepth();

    std::puts("Install ReShade on this exe (DirectX 10/11/12).\n"
              "Waiting for Roblox...");
    std::printf("%ls: toggle input capture. ReShade keeps its own menu and effect shortcuts.\n", g.inputHotkey.c_str());
    if (hotkeys.overlay.key)
        std::printf("%ls: toggle overlay and frame capture.\n", g.overlayHotkey.c_str());

    ULONGLONG nextSearch = 0;
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                ShutdownDepth();
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
            std::puts("Roblox closed. Waiting for Roblox...");
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
                    std::printf("Could not capture Roblox: %ls\n", e.message().c_str());
                }
            }
        }

        UpdateOverlay();

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
                    std::printf("Roblox resized to %dx%d\n", size.Width, size.Height);
                }
            }

            // Also re-presents on timeout, so the ReShade menu stays responsive if Roblox stops drawing.
            if (g.overlayVisible && g.latestFrame)
                PresentLatestFrame();
        }

        MsgWaitForMultipleObjects(1, &g.frameEvent, FALSE, g.overlayVisible ? 16 : 250, QS_ALLINPUT);
    }
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
        return Run();
    }
    catch (const winrt::hresult_error& e)
    {
        std::printf("Error 0x%08X: %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return 1;
    }
}
