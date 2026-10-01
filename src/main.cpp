// Unishade redraws the target game window in its own D3D11 swapchain, so ReShade can be
// installed on this exe. The game is only observed from outside, through window
// enumeration, executable metadata and Windows.Graphics.Capture. No game memory is read or code injected.

#include "addon.h"
#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "launcher.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "reshade_config.h"
#include "game_integration.h"
#include "setup_check.h"
#include "state.h"
#include "update.h"

#include <algorithm>
#include <map>
#include <optional>

State g;

namespace
{
// While nothing is attached, every window is looked at this often. While a game is attached, only the window in front.
constexpr ULONGLONG kSearchInterval = 1000;
// A capture that fails to start is tried again after this, doubling each time up to kMaxCaptureRetry.
constexpr ULONGLONG kFirstCaptureRetry = 2000;
constexpr ULONGLONG kMaxCaptureRetry = 30000;
// The same frame is shown again this often, so toasts and hints still change over a paused game.
constexpr ULONGLONG kRepeatInterval = 100;
// After a shortcut, frames are shown on every wake-up for this long, so what it changed shows right away.
constexpr ULONGLONG kShortcutResponse = 1000;

struct CaptureRetry
{
    ULONGLONG at = 0;
    ULONGLONG delay = 0;
    unsigned failures = 0;
};

struct Loop
{
    ULONGLONG nextSearch = 0;
    HWND lastForeground = nullptr;
    HWND lastTarget = nullptr;
    std::map<HWND, CaptureRetry> captureRetries;

    ULONGLONG nextRepeat = 0;
    ULONGLONG fastUntil = 0;
    bool presentPending = false;
    bool wasInteractive = false;
    bool wasVisible = false;
};
Loop loop;

void ShowError(const std::wstring& message)
{
    MessageBoxW(g.launcher, (message + L"\n\nMore details are in " + LogPath() + L".").c_str(), L"Unishade", MB_ICONERROR);
}

// A damaged list is kept beside it, since the next change to the list replaces the file.
void LoadGames()
{
    const std::wstring path = ExeDirectory() + L"games.ini";
    try
    {
        g.autoGames = LoadAutoGames(path);
    }
    catch (const std::exception& e)
    {
        g.autoGames = DefaultAutoGames();
        if (CopyFileW(path.c_str(), (path + L".damaged").c_str(), FALSE))
            Report(LogLevel::Warning, L"Your saved games could not be loaded (%hs), so Unishade is using the default list. The old list "
                                      L"was kept as games.ini.damaged.",
                   e.what());
        else
            Report(LogLevel::Warning, L"Your saved games could not be loaded (%hs), so Unishade is using the default list. Changing "
                                      L"the list replaces games.ini.",
                   e.what());
    }
}

// A window whose capture fails to start is tried again less and less often, and the failure is logged once.
void Attach(const GameWindow& game)
{
    const ULONGLONG now = GetTickCount64();
    if (const auto retry = loop.captureRetries.find(game.window); retry != loop.captureRetries.end() && now < retry->second.at)
        return;
    if (g.target)
        StopCapture();
    g.activeGame = game;
    try
    {
        StartCapture(game.window);
        loop.captureRetries.erase(game.window);
    }
    catch (const winrt::hresult_error& e)
    {
        StopCapture();
        CaptureRetry& retry = loop.captureRetries[game.window];
        retry.delay = retry.delay ? std::min(retry.delay * 2, kMaxCaptureRetry) : kFirstCaptureRetry;
        retry.at = now + retry.delay;
        if (++retry.failures == 1)
            Log(LogLevel::Error, L"Could not capture %ls: %ls (0x%08X)", game.name.c_str(), e.message().c_str(), static_cast<unsigned>(e.code()));
        else if (retry.failures == 2)
            Log(LogLevel::Info, L"Still could not capture %ls. Trying again less often, up to every %llu seconds.", game.name.c_str(),
                kMaxCaptureRetry / 1000);
    }
}

// Looks at every window only while nothing is attached. An attached game in front needs no search, and another saved
// game coming to the front takes over, so only the window in front is checked then.
void SearchForGame()
{
    // A game that was let go of may be followed by another right away.
    if (!g.target && loop.lastTarget)
        loop.nextSearch = 0;
    loop.lastTarget = g.target;
    const HWND foreground = GetForegroundWindow();
    const bool foregroundChanged = foreground != loop.lastForeground;
    loop.lastForeground = foreground;
    if (g.target && (g.selectedGame || g.editMode))
        return;

    const ULONGLONG now = GetTickCount64();
    std::optional<GameWindow> game;
    if (!g.target && g.selectedGame)
        game = FindGameTarget(g.selectedGame, g.autoGames);
    else if (!g.target)
    {
        if (foregroundChanged)
            game = MatchGameWindow(foreground, g.autoGames);
        if (!game && now >= loop.nextSearch)
        {
            loop.nextSearch = now + kSearchInterval;
            std::erase_if(loop.captureRetries, [](const auto& retry) { return !IsWindow(retry.first); });
            game = FindGameTarget(std::nullopt, g.autoGames, foreground);
        }
    }
    else if (foreground != g.target && (foregroundChanged || now >= loop.nextSearch))
    {
        loop.nextSearch = now + kSearchInterval;
        game = MatchGameWindow(foreground, g.autoGames);
    }
    if (game && game->window != g.target)
        Attach(*game);
}

// Effects run on every present, so the same picture is not shown over and over: a new frame is shown when it arrives,
// and frames are shown on every wake-up only while the menu takes input, ReShade makes effects or a shortcut was just
// used. Otherwise the last frame is shown again every kRepeatInterval.
void ShowFrames()
{
    // Only the newest frame matters. Rendering every queued frame would add latency.
    bool fresh = false;
    while (auto frame = g.pool.TryGetNextFrame())
    {
        g.latestFrame = frame;
        fresh = true;
    }

    if (g.latestFrame)
    {
        SizeInt32 size = g.latestFrame.ContentSize();
        if ((size.Width != g.poolSize.Width || size.Height != g.poolSize.Height) && size.Width > 0 && size.Height > 0)
        {
            g.latestFrame = nullptr;
            g.poolSize = size;
            g.pool.Recreate(g.captureDevice, kPixelFormat, 2, size);
            Log(LogLevel::Info, L"%ls resized to %dx%d", g.activeGame->name.c_str(), size.Width, size.Height);
        }
    }

    // Opening or closing a menu and showing the overlay change the picture without a new frame.
    const bool interactive = g.editMode || ReShadeMenuOpen();
    if (interactive != loop.wasInteractive || (g.overlayVisible && !loop.wasVisible))
        loop.presentPending = true;
    loop.wasInteractive = interactive;
    loop.wasVisible = g.overlayVisible;
    if (!g.overlayVisible || !g.latestFrame)
        return;

    const ULONGLONG now = GetTickCount64();
    if (fresh || interactive || loop.presentPending || ReShadeLoadingEffects() || now < loop.fastUntil || now >= loop.nextRepeat)
    {
        PresentLatestFrame();
        loop.presentPending = false;
        loop.nextRepeat = now + kRepeatInterval;
    }
}

int Run()
{
    if (!GraphicsCaptureSession::IsSupported())
    {
        const wchar_t* message = L"Windows Graphics Capture is not available, and Unishade needs it to copy the game's picture. "
                                 L"Update Windows and your graphics driver.";
        Log(LogLevel::Error, L"%ls", message);
        ShowError(message);
        return 1;
    }
    RequestBorderlessCapture();

    LoadInputHotkeys();
    LoadGames();
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
    Log(LogLevel::Info, L"Waiting for a supported game...");

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
            if (msg.message == WM_HOTKEY)
                loop.fastUntil = GetTickCount64() + kShortcutResponse;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g.captureEnabled && g.target)
            StopCapture();

        if (g.target && !GameWindowExists(*g.activeGame))
        {
            Log(LogLevel::Info, L"%ls closed.", g.activeGame->name.c_str());
            StopCapture();
        }

        if (g.captureEnabled)
            SearchForGame();

        UpdateOverlay();
        if (!g.overlayVisible)
            g.frameStatistics.Reset(FrameStatistics::Clock::now(), g.capturedFrames.load(std::memory_order_relaxed));
        UpdateInputHotkey();
        UpdateHeldCompare();
        UpdateLauncher();

        if (g.target)
            ShowFrames();

        MsgWaitForMultipleObjects(1, &g.frameEvent, FALSE, g.overlayVisible ? 16 : 250, QS_ALLINPUT);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Keep the original mutex name so old and new hosts cannot run together.
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
        Log(LogLevel::Error, L"Unishade stopped: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
        ShowError(L"Unishade stopped because of an error: " + std::wstring(e.message()));
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Unishade stopped: %hs", e.what());
        ShowError(L"Unishade stopped because of an error.");
    }
    CloseHandle(instance);
    FlushLog();
    // Releasing the swapchain makes ReShade wait for the effects it is still compiling, which can take minutes
    // right after installing. ReShade writes settings and presets a second after they change, so the host exits
    // without that wait.
    ExitProcess(static_cast<UINT>(result));
}
