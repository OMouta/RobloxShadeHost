// RobloxShadeHost: redraws the Roblox window in a D3D11 swapchain of its own, so ReShade can be
// installed on this exe instead of Roblox. Roblox is only observed from outside, through window
// enumeration and Windows.Graphics.Capture. Nothing is opened, read or loaded into its process.

#include "addon.h"
#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "launcher.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "reshade_config.h"
#include "roblox_window.h"
#include "setup_check.h"
#include "state.h"
#include "update.h"

State g;

namespace
{
void ShowError(const std::wstring& message)
{
    MessageBoxW(g.launcher, (message + L"\n\nMore details are in " + LogPath() + L".").c_str(), L"RobloxShadeHost", MB_ICONERROR);
}

int Run()
{
    if (!GraphicsCaptureSession::IsSupported())
    {
        const wchar_t* message = L"Windows Graphics Capture is not available, and RobloxShadeHost needs it to copy Roblox's picture. "
                                 L"Update Windows and your graphics driver.";
        Log(LogLevel::Error, L"%ls", message);
        ShowError(message);
        return 1;
    }

    LoadInputHotkeys();
    if (ReShadeLoaded())
        PrepareReShadeConfig();
    CreateOverlayWindow();
    InitAddon();
    InitMenu();
    g.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateDevice();
    CheckSetup();
    InitDepth();
    RegisterHotkeys();
    CheckForUpdate();
    CreateLauncher();
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
                DestroyLauncher();
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
        UpdateInputHotkey();
        UpdateLauncher();

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

            // Also re-presents on timeout, so the menu stays responsive if Roblox stops drawing.
            if (g.overlayVisible && g.latestFrame)
                PresentLatestFrame();
        }

        MsgWaitForMultipleObjects(1, &g.frameEvent, FALSE, g.overlayVisible ? 16 : 250, QS_ALLINPUT);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // A second copy would fight the first over Roblox and the shortcuts, so it shows the first one instead.
    const HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\RobloxShadeHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const HWND other = FindWindowW(kLauncherClass, nullptr))
        {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

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
        ShowError(L"RobloxShadeHost stopped because of an error: " + std::wstring(e.message()));
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"RobloxShadeHost stopped: %hs", e.what());
        ShowError(L"RobloxShadeHost stopped because of an error.");
    }
    CloseHandle(instance);
    // Releasing the swapchain makes ReShade wait for the effects it is still compiling, which can take minutes
    // right after installing. ReShade writes settings and presets a second after they change, so the host exits
    // without that wait.
    ExitProcess(static_cast<UINT>(result));
}
