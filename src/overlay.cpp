#include "overlay.h"
#include "addon.h"
#include "log.h"
#include "menu.h"
#include "state.h"

#include <dwmapi.h>

namespace
{
// WS_EX_LAYERED + WS_EX_TRANSPARENT is what makes clicks reach Roblox. Returning HTTRANSPARENT from
// WM_NCHITTEST only passes input to windows owned by the same thread.
constexpr DWORD kPassThroughStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
constexpr DWORD kEditStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;

void OpenMenu()
{
    if (g.editMode || !g.captureEnabled || !g.target || IsIconic(g.target))
        return;
    SetEditMode(true);
    UpdateOverlay();
    SetForegroundWindow(g.overlay);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_HOTKEY:
        switch (wParam)
        {
        case kOverlayToggleHotkey:
            ToggleOverlay();
            break;
        case kEditModeHotkey:
            if (g.editMode)
                ReturnToRoblox();
            else
                OpenMenu();
            break;
        case kCompareHotkey:
            StartHeldCompare(g.hotkeys.compare.key);
            break;
        case kScreenshotHotkey:
        case kBeforeAfterHotkey:
            RequestScreenshot(wParam == kBeforeAfterHotkey);
            break;
        case kNextPresetHotkey:
        case kPreviousPresetHotkey:
            RequestPresetStep(wParam == kNextPresetHotkey ? 1 : -1);
            break;
        }
        return 0;
    case kLeaveMenuMessage:
        if (g.editMode)
            ReturnToRoblox();
        return 0;
    case kOpenMenuMessage:
        OpenMenu();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            SetEditMode(false);
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g.editMode)
        {
            SetCursor(LoadCursorW(nullptr, MenuCursor()));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

void CreateOverlayWindow()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Setup finds the running host by this class and closes it with WM_CLOSE.
    wc.lpszClassName = L"RobloxShadeHost";
    RegisterClassW(&wc);

    g.overlay = CreateWindowExW(kPassThroughStyle, wc.lpszClassName, L"RobloxShadeHost", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                wc.hInstance, nullptr);
    winrt::check_bool(g.overlay != nullptr);
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);
}

void SetEditMode(bool enabled)
{
    if (enabled == g.editMode)
        return;
    g.editMode = enabled;
    SetWindowLongPtrW(g.overlay, GWL_EXSTYLE, enabled ? kEditStyle : kPassThroughStyle);
    SetWindowPos(g.overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!enabled)
    {
        OpenReShadeMenu(false);
        ResetMenu();
    }
    Log(LogLevel::Info, !enabled ? L"Input returned to Roblox." : AddonRegistered() ? L"Menu opened." : L"Input captured.");
}

void ReturnToRoblox()
{
    SetEditMode(false);
    SetForegroundWindow(g.target);
}

void ToggleOverlay()
{
    g.captureEnabled = !g.captureEnabled;
    if (!g.captureEnabled && g.editMode)
        ReturnToRoblox();
    UpdateOverlay();
    Log(LogLevel::Info, g.captureEnabled ? L"Overlay on." : L"Overlay off. Frame capture stopped.");
}

void UpdateOverlay()
{
    RECT bounds{};
    bool visible = g.captureEnabled && g.target && IsWindowVisible(g.target) && !IsIconic(g.target) &&
                   (g.editMode || GetForegroundWindow() == g.target) &&
                   SUCCEEDED(DwmGetWindowAttribute(g.target, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)));

    if (!visible)
    {
        if (g.overlayVisible)
            ShowWindow(g.overlay, SW_HIDE);
        g.overlayVisible = false;
        return;
    }

    if (!g.overlayVisible || !EqualRect(&bounds, &g.overlayRect))
    {
        SetWindowPos(g.overlay, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g.overlayRect = bounds;
        g.overlayVisible = true;
    }
}
