#include "overlay.h"
#include "addon.h"
#include "log.h"
#include "state.h"

#include <dwmapi.h>

#include <algorithm>

namespace
{
// WS_EX_LAYERED + WS_EX_TRANSPARENT is what makes clicks reach Roblox. Returning HTTRANSPARENT from
// WM_NCHITTEST only passes input to windows owned by the same thread.
constexpr DWORD kPassThroughStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
constexpr DWORD kEditStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;

LRESULT CALLBACK IndicatorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HBRUSH background = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(dc, &rect, background);
        DeleteObject(background);
        HFONT font = CreateFontW(-MulDiv(14, GetDpiForWindow(hwnd), 96), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ previous = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 218, 128));
        DrawTextW(dc, g.indicatorText.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
        DeleteObject(font);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_HOTKEY:
        if (wParam == kOverlayToggleHotkey)
        {
            g.captureEnabled = !g.captureEnabled;
            if (!g.captureEnabled && g.editMode)
            {
                SetEditMode(false);
                SetForegroundWindow(g.target);
            }
            UpdateOverlay();
            Log(LogLevel::Info, g.captureEnabled ? L"Overlay on." : L"Overlay off. Frame capture stopped.");
            return 0;
        }
        if (wParam != kEditModeHotkey)
            break;
        if (g.editMode)
        {
            SetEditMode(false);
            SetForegroundWindow(g.target);
        }
        else if (g.captureEnabled && g.target && !IsIconic(g.target))
        {
            SetEditMode(true);
            UpdateOverlay();
            SetForegroundWindow(hwnd);
        }
        return 0;
    case kMenuClosedMessage:
        if (g.editMode)
        {
            SetEditMode(false);
            SetForegroundWindow(g.target);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            SetEditMode(false);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

void CreateOverlayWindows()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RobloxShadeHost";
    RegisterClassW(&wc);

    g.overlay = CreateWindowExW(kPassThroughStyle, wc.lpszClassName, L"RobloxShadeHost", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                wc.hInstance, nullptr);
    winrt::check_bool(g.overlay != nullptr);
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);

    WNDCLASSW indicatorClass{};
    indicatorClass.lpfnWndProc = IndicatorWndProc;
    indicatorClass.hInstance = wc.hInstance;
    indicatorClass.lpszClassName = L"RobloxShadeHostInputIndicator";
    winrt::check_bool(RegisterClassW(&indicatorClass));
    g.indicator = CreateWindowExW(kPassThroughStyle, indicatorClass.lpszClassName, L"Input captured", WS_POPUP,
                                  0, 0, 1, 1, g.overlay, nullptr, wc.hInstance, nullptr);
    winrt::check_bool(g.indicator != nullptr);
    winrt::check_bool(SetLayeredWindowAttributes(g.indicator, 0, 255, LWA_ALPHA));
}

void SetEditMode(bool enabled)
{
    if (enabled == g.editMode)
        return;
    g.editMode = enabled;
    SetWindowLongPtrW(g.overlay, GWL_EXSTYLE, enabled ? kEditStyle : kPassThroughStyle);
    SetWindowPos(g.overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!enabled)
        ShowWindow(g.indicator, SW_HIDE);
    OpenReShadeMenu(enabled);
    if (enabled && AddonRegistered())
        Log(LogLevel::Info, L"ReShade menu opened. %ls returns to Roblox.", g.inputHotkey.c_str());
    else if (enabled)
        Log(LogLevel::Info, L"Input captured. Open the menu with ReShade's own key; %ls returns to Roblox.", g.inputHotkey.c_str());
    else
        Log(LogLevel::Info, L"Input returned to Roblox.");
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
        ShowWindow(g.indicator, SW_HIDE);
        return;
    }

    if (!g.overlayVisible || !EqualRect(&bounds, &g.overlayRect))
    {
        SetWindowPos(g.overlay, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g.overlayRect = bounds;
        g.overlayVisible = true;
    }
    if (g.editMode)
    {
        const UINT dpi = GetDpiForWindow(g.overlay);
        const int margin = MulDiv(12, dpi, 96);
        const int height = MulDiv(36, dpi, 96);
        // Keep the badge above the swapchain, without taking focus or blocking clicks.
        const int width = std::min<int>(bounds.right - bounds.left, MulDiv(560, dpi, 96));
        SetWindowPos(g.indicator, HWND_TOPMOST, bounds.left + (bounds.right - bounds.left - width) / 2,
                     bounds.bottom - height - margin, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}
