// GDI+ comes first: its headers name parameters g, like the host's global state.
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus
{
// GDI+ uses min and max, which NOMINMAX removes.
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include "launcher.h"
#include "addon.h"
#include "config.h"
#include "log.h"
#include "resource.h"
#include "shell.h"
#include "state.h"
#include "theme.h"
#include "update.h"

#include <dwmapi.h>
#include <shlwapi.h>

#include <cmath>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
// Width in pixels at 100% scaling. The height follows the content.
constexpr float kWidth = 460;

enum class Action
{
    OpenLog,
    Help,
    RunSetup,
    Download,
};

struct Link
{
    RECT rect;
    Action action;
    const wchar_t* label;
};

struct Row
{
    RECT rect;
    LogLevel level;
    std::wstring text;
};

struct Launcher
{
    ULONG_PTR gdiplus = 0;
    winrt::com_ptr<IStream> logoStream;
    std::unique_ptr<Gdiplus::Bitmap> logo;
    float scale = 1;
    HFONT title = nullptr;
    HFONT semibold = nullptr;
    HFONT body = nullptr;

    // What is shown, compared every loop to know when to redraw.
    std::wstring shown;
    std::wstring statusTitle;
    std::wstring statusDetail;
    unsigned statusColor = 0;
    Update update;
    std::wstring setup;

    // Layout in client pixels.
    int height = 0;
    RECT logoRect{};
    RECT statusCard{};
    RECT statusTitleRect{};
    RECT statusDetailRect{};
    RECT updateCard{};
    RECT updateText{};
    std::vector<Row> rows;
    int footer = 0;
    std::vector<Link> links;
    int hovered = -1;
    bool tracking = false;
};
Launcher l;

// Pixels at the window's scaling, whole for GDI and fractional for GDI+.
int P(float value)
{
    return static_cast<int>(std::lround(value * l.scale));
}

float F(float value)
{
    return value * l.scale;
}

float F(LONG pixels)
{
    return static_cast<float>(pixels);
}

COLORREF Gdi(unsigned rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

Gdiplus::Color Plus(unsigned rgb, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

HFONT Font(float size, bool semibold)
{
    return CreateFontW(-P(size), 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, semibold ? L"Segoe UI Semibold" : L"Segoe UI");
}

void CreateFonts()
{
    for (HFONT font : { l.title, l.semibold, l.body })
        if (font)
            DeleteObject(font);
    l.title = Font(19, true);
    l.semibold = Font(15, true);
    l.body = Font(13.5f, false);
}

int TextHeight(HDC dc, HFONT font, const std::wstring& text, int width)
{
    RECT rect{ 0, 0, width, 0 };
    const HGDIOBJ previous = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, previous);
    return rect.bottom;
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text)
{
    RECT rect{};
    const HGDIOBJ previous = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
    return rect.right;
}

void Describe()
{
    if (!g.captureEnabled)
    {
        l.statusTitle = L"Overlay off";
        l.statusDetail = g.hotkeys.overlay.key ? L"Press " + g.overlayHotkey + L" in Roblox to turn it back on." : L"";
        l.statusColor = theme::kDim;
    }
    else if (g.target)
    {
        l.statusTitle = L"Running on Roblox";
        l.statusDetail = AddonRegistered() ? L"Press " + g.inputHotkey + L" in Roblox to open the menu." : L"The menu is off. See below.";
        l.statusColor = theme::kSuccess;
    }
    else
    {
        l.statusTitle = L"Waiting for Roblox";
        l.statusDetail = L"Open a Roblox experience. The overlay starts by itself.";
        l.statusColor = theme::kAccent;
    }
    l.update = AvailableUpdate();
    const std::wstring setup = ExeDirectory() + L"RobloxShadeHost-Setup.exe";
    l.setup = GetFileAttributesW(setup.c_str()) != INVALID_FILE_ATTRIBUTES ? setup : L"";
}

void Layout(HDC dc)
{
    const int width = P(kWidth);
    const int pad = P(24);
    int y = P(3) + P(22);

    l.logoRect = { pad, y, pad + P(52), y + P(52) };
    y += P(52) + P(20);

    const int inner = P(16);
    const int textLeft = pad + inner + P(22);
    const int textWidth = width - pad - inner - textLeft;
    const int titleHeight = TextHeight(dc, l.semibold, l.statusTitle, textWidth);
    const int detailHeight = l.statusDetail.empty() ? 0 : TextHeight(dc, l.body, l.statusDetail, textWidth);
    l.statusTitleRect = { textLeft, y + inner, textLeft + textWidth, y + inner + titleHeight };
    l.statusDetailRect = { textLeft, l.statusTitleRect.bottom + P(3), textLeft + textWidth, l.statusTitleRect.bottom + P(3) + detailHeight };
    l.statusCard = { pad, y, width - pad, l.statusDetailRect.bottom + inner };
    y = l.statusCard.bottom + P(14);

    l.links.clear();
    l.updateCard = {};
    if (!l.update.version.empty())
    {
        l.updateCard = { pad, y, width - pad, y + P(44) };
        const std::wstring download = L"Download";
        const int downloadWidth = TextWidth(dc, l.semibold, download);
        const RECT downloadRect{ l.updateCard.right - inner - downloadWidth, y, l.updateCard.right - inner, l.updateCard.bottom };
        l.links.push_back({ downloadRect, Action::Download, L"Download" });
        l.updateText = { pad + inner, y, downloadRect.left - P(8), l.updateCard.bottom };
        y = l.updateCard.bottom + P(14);
    }

    l.rows.clear();
    for (const Notice& notice : Notices())
    {
        const int left = pad + P(26);
        const int height = std::max(TextHeight(dc, l.body, notice.text, width - pad - left), P(18));
        l.rows.push_back({ { left, y, width - pad, y + height }, notice.level, notice.text });
        y += height + P(8);
    }

    y += P(6);
    l.footer = y;
    const int linkY = y + P(14);
    int x = pad;
    const auto link = [&](Action action, const wchar_t* label) {
        const int linkWidth = TextWidth(dc, l.body, label);
        l.links.push_back({ { x, linkY, x + linkWidth, linkY + P(20) }, action, label });
        x += linkWidth + P(20);
    };
    link(Action::OpenLog, L"Open log");
    link(Action::Help, L"Get help on Discord");
    if (!l.setup.empty())
        link(Action::RunSetup, L"Run Setup");
    l.height = linkY + P(20) + P(16);
}

// Sizes the window to its content, keeping it where it is and on screen.
void Resize()
{
    const HDC dc = GetDC(g.launcher);
    Layout(dc);
    ReleaseDC(g.launcher, dc);

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.launcher, GWL_STYLE));
    const UINT dpi = GetDpiForWindow(g.launcher);
    RECT frame{ 0, 0, P(kWidth), l.height };
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(g.launcher, MONITOR_DEFAULTTONEAREST), &monitor);
    const int height = std::min<int>(frame.bottom - frame.top, monitor.rcWork.bottom - monitor.rcWork.top);
    SetWindowPos(g.launcher, nullptr, 0, 0, frame.right - frame.left, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g.launcher, nullptr, FALSE);
}

void FillRounded(Gdiplus::Graphics& graphics, const RECT& rect, float radius, const Gdiplus::Color& fill, const Gdiplus::Color& border)
{
    Gdiplus::GraphicsPath path;
    const float x = F(rect.left) + 0.5f, y = F(rect.top) + 0.5f, w = F(rect.right - rect.left) - 1.0f, h = F(rect.bottom - rect.top) - 1.0f;
    const float d = radius * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(fill);
    graphics.FillPath(&brush, &path);
    Gdiplus::Pen pen(border, 1.0f);
    graphics.DrawPath(&pen, &path);
}

void Icon(Gdiplus::Graphics& graphics, Gdiplus::PointF center, LogLevel level)
{
    const unsigned color = level == LogLevel::Ok ? theme::kSuccess : level == LogLevel::Warning ? theme::kWarning
                         : level == LogLevel::Error ? theme::kError : theme::kDim;
    const float r = F(8.0f);
    const float s = l.scale;
    Gdiplus::SolidBrush tint(Plus(color, 0x30));
    Gdiplus::SolidBrush solid(Plus(color));
    Gdiplus::Pen pen(Plus(color), 1.6f * s);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.FillEllipse(&tint, center.X - r, center.Y - r, r * 2, r * 2);
    switch (level)
    {
    case LogLevel::Ok:
    {
        const Gdiplus::PointF points[] = { { center.X - 3.5f * s, center.Y }, { center.X - 1.0f * s, center.Y + 2.8f * s }, { center.X + 3.8f * s, center.Y - 2.8f * s } };
        graphics.DrawLines(&pen, points, 3);
        break;
    }
    case LogLevel::Warning:
        graphics.DrawLine(&pen, center.X, center.Y - 4 * s, center.X, center.Y + 1 * s);
        graphics.FillEllipse(&solid, center.X - 1.1f * s, center.Y + 2.7f * s, 2.2f * s, 2.2f * s);
        break;
    case LogLevel::Error:
        graphics.DrawLine(&pen, center.X - 3 * s, center.Y - 3 * s, center.X + 3 * s, center.Y + 3 * s);
        graphics.DrawLine(&pen, center.X + 3 * s, center.Y - 3 * s, center.X - 3 * s, center.Y + 3 * s);
        break;
    default:
        graphics.FillEllipse(&solid, center.X - 2.2f * s, center.Y - 2.2f * s, 4.4f * s, 4.4f * s);
        break;
    }
}

void PaintText(HDC dc, HFONT font, unsigned color, const std::wstring& text, RECT rect, UINT format = DT_WORDBREAK)
{
    SelectObject(dc, font);
    SetTextColor(dc, Gdi(color));
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
}

void Paint(HDC target)
{
    RECT client{};
    GetClientRect(g.launcher, &client);
    const int width = client.right;
    const int height = client.bottom;
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    const HGDIOBJ previousFont = SelectObject(dc, l.body);
    Layout(dc);

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        Gdiplus::SolidBrush background(Plus(theme::kBackground));
        graphics.FillRectangle(&background, 0, 0, width, height);

        // The logo's ring as a strip along the top, like Setup.
        constexpr int count = static_cast<int>(std::size(theme::kRainbow));
        Gdiplus::Color colors[count];
        Gdiplus::REAL positions[count];
        for (int i = 0; i < count; ++i)
        {
            colors[i] = Plus(theme::kRainbow[i]);
            positions[i] = static_cast<Gdiplus::REAL>(i) / (count - 1);
        }
        Gdiplus::LinearGradientBrush strip(Gdiplus::Point(0, 0), Gdiplus::Point(width, 0), colors[0], colors[count - 1]);
        strip.SetInterpolationColors(colors, positions, count);
        graphics.FillRectangle(&strip, 0, 0, width, P(3));

        if (l.logo)
            graphics.DrawImage(l.logo.get(), F(l.logoRect.left), F(l.logoRect.top), F(l.logoRect.right - l.logoRect.left),
                               F(l.logoRect.bottom - l.logoRect.top));

        FillRounded(graphics, l.statusCard, F(10.0f), Plus(theme::kCard), Plus(theme::kBorder));
        const Gdiplus::PointF dot(F(l.statusCard.left) + F(22.0f), F(l.statusTitleRect.top + l.statusTitleRect.bottom) / 2);
        Gdiplus::SolidBrush glow(Plus(l.statusColor, 0x40));
        Gdiplus::SolidBrush solid(Plus(l.statusColor));
        graphics.FillEllipse(&glow, dot.X - F(8.0f), dot.Y - F(8.0f), F(16.0f), F(16.0f));
        graphics.FillEllipse(&solid, dot.X - F(4.5f), dot.Y - F(4.5f), F(9.0f), F(9.0f));

        if (!l.update.version.empty())
            FillRounded(graphics, l.updateCard, F(10.0f), Plus(theme::kAccent, 0x22), Plus(theme::kAccent, 0x90));

        for (const Row& row : l.rows)
            Icon(graphics, Gdiplus::PointF(F(row.rect.left) - F(18.0f), F(row.rect.top) + F(9.0f)), row.level);

        Gdiplus::Pen line(Plus(theme::kBorder), 1.0f);
        graphics.DrawLine(&line, 0, l.footer, width, l.footer);
    }

    SetBkMode(dc, TRANSPARENT);
    PaintText(dc, l.title, theme::kText, L"RobloxShadeHost",
             { l.logoRect.right + P(14), l.logoRect.top + P(4), width, l.logoRect.top + P(32) }, DT_SINGLELINE);
    PaintText(dc, l.body, theme::kDim, L"Version " ROBLOX_SHADE_HOST_VERSION,
             { l.logoRect.right + P(14), l.logoRect.top + P(30), width, l.logoRect.bottom }, DT_SINGLELINE);
    PaintText(dc, l.semibold, theme::kText, l.statusTitle, l.statusTitleRect);
    PaintText(dc, l.body, theme::kDim, l.statusDetail, l.statusDetailRect);
    if (!l.update.version.empty())
        PaintText(dc, l.body, theme::kText, L"RobloxShadeHost " + l.update.version + L" is available.", l.updateText,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    for (const Row& row : l.rows)
        PaintText(dc, l.body, theme::kText, row.text, row.rect);
    for (size_t i = 0; i < l.links.size(); ++i)
    {
        const Link& link = l.links[i];
        const bool hovered = l.hovered == static_cast<int>(i);
        if (link.action == Action::Download)
            PaintText(dc, l.semibold, hovered ? theme::kText : theme::kAccentHover, link.label, link.rect, DT_SINGLELINE | DT_VCENTER);
        else
            PaintText(dc, l.body, hovered ? theme::kText : theme::kAccentHover, link.label, link.rect, DT_SINGLELINE);
    }

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

int LinkAt(LPARAM lParam)
{
    const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
    for (size_t i = 0; i < l.links.size(); ++i)
        if (PtInRect(&l.links[i].rect, point))
            return static_cast<int>(i);
    return -1;
}

void Run(Action action)
{
    switch (action)
    {
    case Action::OpenLog: ShellOpen(LogPath()); break;
    case Action::Help: ShellOpen(kHelpUrl); break;
    case Action::RunSetup: ShellOpen(l.setup); break;
    case Action::Download: ShellOpen(l.update.url); break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        Paint(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        if (!l.tracking)
        {
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
            l.tracking = TrackMouseEvent(&track);
        }
        const int hovered = LinkAt(lParam);
        if (hovered != l.hovered)
        {
            l.hovered = hovered;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        l.tracking = false;
        if (l.hovered >= 0)
        {
            l.hovered = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && l.hovered >= 0)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        if (const int link = LinkAt(lParam); link >= 0)
            Run(l.links[link].action);
        return 0;
    case WM_DPICHANGED:
    {
        l.scale = HIWORD(wParam) / 96.0f;
        CreateFonts();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Resize();
        return 0;
    }
    case WM_DESTROY:
        g.launcher = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void LoadLogo()
{
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    const HGLOBAL resource = info ? LoadResource(nullptr, info) : nullptr;
    if (!resource)
        return;
    l.logoStream.attach(SHCreateMemStream(static_cast<const BYTE*>(LockResource(resource)), SizeofResource(nullptr, info)));
    if (!l.logoStream)
        return;
    l.logo.reset(Gdiplus::Bitmap::FromStream(l.logoStream.get()));
    if (l.logo && l.logo->GetLastStatus() != Gdiplus::Ok)
        l.logo.reset();
}
} // namespace

void CreateLauncher()
{
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&l.gdiplus, &input, nullptr);
    LoadLogo();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kLauncherClass;
    RegisterClassExW(&wc);
    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g.launcher = CreateWindowExW(0, kLauncherClass, L"RobloxShadeHost", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    winrt::check_bool(g.launcher != nullptr);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g.launcher, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = Gdi(theme::kBackground);
    DwmSetWindowAttribute(g.launcher, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    l.scale = GetDpiForWindow(g.launcher) / 96.0f;
    CreateFonts();
    Describe();
    Resize();

    // Centered on the monitor the window opened on.
    RECT window{};
    GetWindowRect(g.launcher, &window);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(g.launcher, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    SetWindowPos(g.launcher, nullptr, (monitor.rcWork.left + monitor.rcWork.right - width) / 2,
                 (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g.launcher, SW_SHOWNORMAL);
}

void UpdateLauncher()
{
    if (!g.launcher)
        return;
    const Update update = AvailableUpdate();
    const std::wstring shown = std::to_wstring(g.captureEnabled) + L"|" + std::to_wstring(g.target != nullptr) + L"|" +
                               std::to_wstring(NoticeVersion()) + L"|" + update.version + L"|" + g.inputHotkey + L"|" + g.overlayHotkey;
    if (shown == l.shown)
        return;
    l.shown = shown;
    Describe();
    Resize();
}

void DestroyLauncher()
{
    if (g.launcher)
        DestroyWindow(g.launcher);
    g.launcher = nullptr;
    for (HFONT font : { l.title, l.semibold, l.body })
        if (font)
            DeleteObject(font);
    l.logo.reset();
    l.logoStream = nullptr;
    if (l.gdiplus)
        Gdiplus::GdiplusShutdown(l.gdiplus);
}
