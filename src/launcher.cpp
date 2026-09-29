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
#include "capture.h"
#include "config.h"
#include "log.h"
#include "resource.h"
#include "shell.h"
#include "state.h"
#include "theme.h"
#include "update.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Size in pixels at 100% scaling. Longer content makes the window taller, up to the screen, and then scrolls.
constexpr float kWidth = 620;
constexpr float kMinHeight = 540;

enum class Action
{
    OpenLog,
    Help,
    RunSetup,
    Download,
    AddGame,
    PickWindow,
    DetectAutomatically,
    ClosePicker,
    ToggleGame,
    RemoveGame,
    UseWindow,
};

enum class Look
{
    Link,
    Button,
    PrimaryButton,
    Switch,
    Remove,
    Row,
};

// Something that can be clicked. index is the game or window it acts on.
struct Target
{
    RECT rect;
    Action action;
    Look look;
    size_t index = 0;
    std::wstring label;
    bool on = false;
};

// A saved game, or an open window in the picker.
struct Entry
{
    std::wstring name;
    std::wstring detail;
    fs::path icon; // the executable, or empty to show the name's first letter
    bool enabled = true;
    bool running = false;
    RECT rect{};
    RECT iconRect{};
    RECT nameRect{};
    RECT detailRect{};
    RECT badge{};
};

struct Row
{
    RECT rect;
    LogLevel level;
    std::wstring text;
};

// Picking a window either saves its game or uses the window until Detect automatically.
enum class Picker
{
    None,
    Add,
    Session,
};

struct Launcher
{
    ULONG_PTR gdiplus = 0;
    winrt::com_ptr<IStream> logoStream;
    std::unique_ptr<Gdiplus::Bitmap> logo;
    float scale = 1;
    HFONT title = nullptr;
    HFONT semibold = nullptr;
    HFONT strong = nullptr;
    HFONT body = nullptr;
    HFONT note = nullptr;
    // By pixel size and executable. nullptr when the executable has no icon.
    std::map<std::wstring, HICON> icons;
    // Where games saved by filename were found running.
    std::map<std::wstring, fs::path> located;

    // What is shown, compared every loop to know when to redraw.
    std::wstring shown;
    std::wstring statusTitle;
    std::wstring statusDetail;
    std::wstring statusName;
    fs::path statusIcon;
    unsigned statusColor = 0;
    fs::path activeExecutable;
    Update update;
    std::wstring setup;
    Picker picker = Picker::None;
    std::vector<GameWindow> windows;
    std::vector<fs::path> windowExecutables;
    std::wstring sectionTitle;
    std::wstring sectionDetail;
    std::vector<Entry> entries;
    std::wstring empty;

    // Layout in client pixels, moved up by scroll.
    int height = 0; // of everything, unscrolled
    int scroll = 0;
    RECT logoRect{};
    RECT statusCard{};
    RECT statusIconRect{};
    RECT statusTitleRect{};
    RECT statusDetailRect{};
    RECT updateCard{};
    RECT updateText{};
    RECT sectionTitleRect{};
    RECT sectionDetailRect{};
    RECT list{};
    RECT setupTitle{};
    std::vector<Row> rows;
    int footer = 0;
    std::vector<Target> targets;
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

void DeleteFonts()
{
    for (HFONT font : { l.title, l.semibold, l.strong, l.body, l.note })
        if (font)
            DeleteObject(font);
}

void CreateFonts()
{
    DeleteFonts();
    l.title = Font(19, true);
    l.semibold = Font(15, true);
    l.strong = Font(13.5f, true);
    l.body = Font(13.5f, false);
    l.note = Font(12, false);
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

// The executable's icon at a size in pixels. nullptr when it has none.
HICON ExecutableIcon(const fs::path& executable, int size)
{
    if (executable.empty())
        return nullptr;
    const std::wstring key = std::to_wstring(size) + L"|" + executable.wstring();
    if (const auto found = l.icons.find(key); found != l.icons.end())
        return found->second;
    HICON icon = nullptr;
    if (SHDefExtractIconW(executable.c_str(), 0, 0, &icon, nullptr, static_cast<UINT>(size)) != S_OK)
        icon = nullptr;
    l.icons[key] = icon;
    return icon;
}

void ClearIcons()
{
    for (const auto& [key, icon] : l.icons)
        if (icon)
            DestroyIcon(icon);
    l.icons.clear();
}

// Empty when the process has exited or denies access, which shows the name's first letter instead of the icon.
fs::path Executable(DWORD processId)
{
    try
    {
        return ProcessExecutable(processId);
    }
    catch (const std::system_error&)
    {
        return {};
    }
}

// Games saved by filename, like Roblox, move with every update, so their icon comes from a running copy.
void LocateGames()
{
    const auto unlocated = [](const AutoGame& game) {
        return !game.executable.has_parent_path() && !l.located.contains(game.executable.wstring());
    };
    if (std::none_of(g.autoGames.begin(), g.autoGames.end(), unlocated))
        return;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W process{ sizeof(process) };
    for (BOOL ok = Process32FirstW(snapshot, &process); ok; ok = Process32NextW(snapshot, &process))
        for (const AutoGame& game : g.autoGames)
            if (unlocated(game) && _wcsicmp(process.szExeFile, game.executable.c_str()) == 0)
                if (fs::path executable = Executable(process.th32ProcessID); !executable.empty())
                    l.located[game.executable.wstring()] = std::move(executable);
    CloseHandle(snapshot);
}

void Describe()
{
    l.activeExecutable = g.activeGame ? Executable(g.activeGame->processId) : fs::path{};
    const std::optional<GameWindow>& game = g.activeGame ? g.activeGame : g.selectedGame;
    l.statusName = game ? game->name : L"";
    l.statusIcon = g.activeGame ? l.activeExecutable : game ? Executable(game->processId) : fs::path{};
    if (!g.captureEnabled)
    {
        l.statusTitle = L"Overlay off";
        l.statusDetail = g.hotkeys.overlay.key ? L"Press " + g.overlayHotkey + L" in the game to turn it back on." : L"";
        l.statusColor = theme::kDim;
    }
    else if (g.target)
    {
        l.statusTitle = L"Running on " + g.activeGame->name;
        l.statusDetail = AddonRegistered() ? L"Press " + g.inputHotkey + L" in the game to open the menu." : L"The menu is off. See below.";
        l.statusColor = theme::kSuccess;
    }
    else if (g.selectedGame)
    {
        l.statusTitle = L"Waiting for " + g.selectedGame->name;
        l.statusDetail = GameWindowExists(*g.selectedGame) ? L"Return to the game to see the effects." : L"Its window closed. Pick its new window.";
        l.statusColor = theme::kAccent;
    }
    else
    {
        l.statusTitle = L"Waiting for a game";
        l.statusDetail = std::any_of(g.autoGames.begin(), g.autoGames.end(), [](const AutoGame& saved) { return saved.enabled; })
                             ? L"Open one of your games."
                             : L"Open a game and choose Add game.";
        l.statusColor = theme::kAccent;
    }

    l.entries.clear();
    switch (l.picker)
    {
    case Picker::None:
        LocateGames();
        for (const AutoGame& saved : g.autoGames)
        {
            const auto located = l.located.find(saved.executable.wstring());
            l.entries.push_back({ .name = saved.name,
                                  .detail = saved.executable.wstring(),
                                  .icon = saved.executable.has_parent_path() ? saved.executable
                                        : located != l.located.end()        ? located->second
                                                                            : fs::path{},
                                  .enabled = saved.enabled,
                                  .running = g.target && MatchesExecutable(saved, l.activeExecutable) });
        }
        l.sectionTitle = L"Games";
        l.sectionDetail.clear();
        l.empty = L"No games yet. Open one and choose Add game.";
        break;
    case Picker::Add:
    case Picker::Session:
        for (size_t i = 0; i < l.windows.size(); ++i)
            l.entries.push_back({ .name = l.windows[i].name, .detail = l.windowExecutables[i].filename().wstring(), .icon = l.windowExecutables[i] });
        l.sectionTitle = l.picker == Picker::Add ? L"Pick the game's window" : L"Pick a window for this session";
        l.sectionDetail = l.picker == Picker::Add ? L"Open the game first if it is not listed."
                                                  : L"Unishade stays on it until you choose Detect automatically.";
        l.empty = L"No open windows.";
        break;
    }

    l.update = AvailableUpdate();
    const std::wstring setup = ExeDirectory() + L"Unishade-Setup.exe";
    l.setup = GetFileAttributesW(setup.c_str()) != INVALID_FILE_ATTRIBUTES ? setup : L"";
}

int ButtonWidth(HDC dc, HFONT font, const wchar_t* label)
{
    return TextWidth(dc, font, label) + P(28);
}

void Layout(HDC dc)
{
    const int width = P(kWidth);
    const int pad = P(28);
    const int right = width - pad;
    const int inner = P(18);
    int y = P(3) + P(24) - l.scroll;
    l.targets.clear();

    l.logoRect = { pad, y, pad + P(44), y + P(44) };
    y += P(44) + P(22);

    // The game's icon, what is happening, and switching between detection and a picked window.
    const int iconSize = P(40);
    const Action statusAction = g.selectedGame ? Action::DetectAutomatically : Action::PickWindow;
    const wchar_t* statusLabel = g.selectedGame ? L"Detect automatically" : L"Pick a window";
    const int buttonWidth = ButtonWidth(dc, l.body, statusLabel);
    const int textLeft = pad + inner + iconSize + P(14);
    const int textRight = right - inner - buttonWidth - P(16);
    const int titleHeight = TextHeight(dc, l.semibold, l.statusTitle, textRight - textLeft);
    const int detailHeight = l.statusDetail.empty() ? 0 : TextHeight(dc, l.body, l.statusDetail, textRight - textLeft);
    const int textHeight = titleHeight + (detailHeight ? P(3) + detailHeight : 0);
    l.statusCard = { pad, y, right, y + std::max(iconSize, textHeight) + inner * 2 };
    int middle = (l.statusCard.top + l.statusCard.bottom) / 2;
    l.statusIconRect = { pad + inner, middle - iconSize / 2, pad + inner + iconSize, middle - iconSize / 2 + iconSize };
    l.statusTitleRect = { textLeft, middle - textHeight / 2, textRight, middle - textHeight / 2 + titleHeight };
    l.statusDetailRect = { textLeft, l.statusTitleRect.bottom + P(3), textRight, l.statusTitleRect.bottom + P(3) + detailHeight };
    l.targets.push_back({ { right - inner - buttonWidth, middle - P(15), right - inner, middle + P(15) }, statusAction, Look::Button, 0, statusLabel });
    y = l.statusCard.bottom + P(14);

    l.updateCard = {};
    if (!l.update.version.empty())
    {
        l.updateCard = { pad, y, right, y + P(44) };
        const std::wstring download = L"Download";
        const int downloadWidth = TextWidth(dc, l.semibold, download);
        const RECT downloadRect{ right - inner - downloadWidth, y, right - inner, l.updateCard.bottom };
        l.targets.push_back({ downloadRect, Action::Download, Look::Link, 0, download });
        l.updateText = { pad + inner, y, downloadRect.left - P(8), l.updateCard.bottom };
        y = l.updateCard.bottom + P(14);
    }

    // The saved games, or the open windows while picking one.
    y += P(12);
    const bool picking = l.picker != Picker::None;
    const wchar_t* sectionLabel = picking ? L"Cancel" : L"Add game";
    const int sectionButton = ButtonWidth(dc, picking ? l.body : l.strong, sectionLabel);
    l.sectionTitleRect = { pad, y, right - sectionButton - P(12), y + P(30) };
    l.targets.push_back({ { right - sectionButton, y, right, y + P(30) }, picking ? Action::ClosePicker : Action::AddGame,
                          picking ? Look::Button : Look::PrimaryButton, 0, sectionLabel });
    y += P(30);
    l.sectionDetailRect = {};
    if (!l.sectionDetail.empty())
    {
        l.sectionDetailRect = { pad, y, right, y + TextHeight(dc, l.body, l.sectionDetail, right - pad) };
        y = l.sectionDetailRect.bottom;
    }
    y += P(12);

    const int rowHeight = P(picking ? 48.0f : 56.0f);
    const int rowIcon = P(picking ? 28.0f : 32.0f);
    l.list = { pad, y, right, y + std::max<int>(1, static_cast<int>(l.entries.size())) * rowHeight };
    for (size_t i = 0; i < l.entries.size(); ++i)
    {
        Entry& entry = l.entries[i];
        const int top = y + static_cast<int>(i) * rowHeight;
        middle = top + rowHeight / 2;
        entry.rect = { pad, top, right, top + rowHeight };
        entry.iconRect = { pad + P(14), middle - rowIcon / 2, pad + P(14) + rowIcon, middle - rowIcon / 2 + rowIcon };
        int nameRight = right - P(14);
        entry.badge = {};
        if (picking)
            l.targets.push_back({ entry.rect, Action::UseWindow, Look::Row, i });
        else
        {
            const RECT remove{ right - P(12) - P(28), middle - P(14), right - P(12), middle + P(14) };
            const RECT toggle{ remove.left - P(10) - P(36), middle - P(10), remove.left - P(10), middle + P(10) };
            l.targets.push_back({ remove, Action::RemoveGame, Look::Remove, i });
            l.targets.push_back({ toggle, Action::ToggleGame, Look::Switch, i, L"", entry.enabled });
            nameRight = toggle.left - P(14);
            if (entry.running)
            {
                entry.badge = { nameRight - TextWidth(dc, l.note, L"Running") - P(18), middle - P(11), nameRight, middle + P(11) };
                nameRight = entry.badge.left - P(12);
            }
        }
        const int nameLeft = entry.iconRect.right + P(12);
        entry.nameRect = { nameLeft, middle - P(20), nameRight, middle };
        entry.detailRect = { nameLeft, middle + P(1), nameRight, middle + P(19) };
        if (entry.detail.empty())
            entry.nameRect = { nameLeft, top, nameRight, top + rowHeight };
    }
    y = l.list.bottom + P(26);

    l.rows.clear();
    l.setupTitle = {};
    const std::vector<Notice> notices = Notices();
    if (!notices.empty())
    {
        l.setupTitle = { pad, y, right, y + P(22) };
        y += P(22) + P(10);
        for (const Notice& notice : notices)
        {
            const int left = pad + P(26);
            const int height = std::max(TextHeight(dc, l.body, notice.text, right - left), P(18));
            l.rows.push_back({ { left, y, right, y + height }, notice.level, notice.text });
            y += height + P(8);
        }
        y += P(8);
    }

    // The links stay at the bottom of the window when the content is shorter.
    l.footer = std::max(y, P(kMinHeight) - P(51) - l.scroll);
    int linkY = l.footer + P(15);
    int x = pad;
    const auto link = [&](Action action, const wchar_t* label) {
        const int linkWidth = TextWidth(dc, l.body, label);
        if (x + linkWidth > right)
        {
            x = pad;
            linkY += P(28);
        }
        l.targets.push_back({ { x, linkY, x + linkWidth, linkY + P(20) }, action, Look::Link, 0, label });
        x += linkWidth + P(20);
    };
    link(Action::OpenLog, L"Open log");
    link(Action::Help, L"Get help on Discord");
    if (!l.setup.empty())
        link(Action::RunSetup, L"Run Setup");
    l.height = linkY + P(20) + P(16) + l.scroll;
}

void Relayout()
{
    const HDC dc = GetDC(g.launcher);
    Layout(dc);
    ReleaseDC(g.launcher, dc);
}

// Sizes the window to its content, keeping it where it is and on screen.
void Resize()
{
    Relayout();
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

int TargetAt(POINT point)
{
    for (size_t i = 0; i < l.targets.size(); ++i)
        if (PtInRect(&l.targets[i].rect, point))
            return static_cast<int>(i);
    return -1;
}

void UpdateHover()
{
    POINT point{};
    GetCursorPos(&point);
    const bool inside = WindowFromPoint(point) == g.launcher;
    ScreenToClient(g.launcher, &point);
    const int hovered = inside ? TargetAt(point) : -1;
    if (hovered != l.hovered)
    {
        l.hovered = hovered;
        InvalidateRect(g.launcher, nullptr, FALSE);
    }
}

std::wstring Shown()
{
    const auto game = [](const std::optional<GameWindow>& window) -> std::wstring {
        if (!window)
            return L"-";
        return std::to_wstring(reinterpret_cast<uintptr_t>(window->window)) + L":" + window->name + L":" +
               std::to_wstring(GameWindowExists(*window));
    };
    return std::to_wstring(g.captureEnabled) + L"|" + std::to_wstring(g.target != nullptr) + L"|" + game(g.activeGame) + L"|" +
           game(g.selectedGame) + L"|" + std::to_wstring(NoticeVersion()) + L"|" + AvailableUpdate().version + L"|" + g.inputHotkey +
           L"|" + g.overlayHotkey;
}

void Refresh()
{
    l.shown = Shown();
    Describe();
    Resize();
    UpdateHover();
}

RECT Inset(RECT rect, int by)
{
    InflateRect(&rect, -by, -by);
    return rect;
}

void FillRounded(Gdiplus::Graphics& graphics, const RECT& rect, float radius, const Gdiplus::Color& fill, const Gdiplus::Color& border)
{
    Gdiplus::GraphicsPath path;
    const float x = F(rect.left) + 0.5f, y = F(rect.top) + 0.5f, w = F(rect.right - rect.left) - 1.0f, h = F(rect.bottom - rect.top) - 1.0f;
    const float d = std::min({ radius * 2, w, h });
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

void PaintIcon(Gdiplus::Graphics& graphics, HICON icon, const RECT& rect)
{
    const HDC dc = graphics.GetHDC();
    DrawIconEx(dc, rect.left, rect.top, icon, rect.right - rect.left, rect.bottom - rect.top, 0, nullptr, DI_NORMAL);
    graphics.ReleaseHDC(dc);
}

// The name's first letter stands in for a missing icon.
std::wstring Initial(const std::wstring& name)
{
    for (wchar_t character : name)
        if (IsCharAlphaNumericW(character))
        {
            wchar_t letter[] = { character, L'\0' };
            CharUpperW(letter);
            return letter;
        }
    return L"?";
}

void LetterTile(Gdiplus::Graphics& graphics, const RECT& rect)
{
    FillRounded(graphics, rect, F(8.0f), Plus(theme::kCardHover), Plus(theme::kBorder));
}

void StatusIcon(Gdiplus::Graphics& graphics)
{
    const RECT& rect = l.statusIconRect;
    Gdiplus::SolidBrush solid(Plus(l.statusColor));
    if (l.statusName.empty())
    {
        const Gdiplus::PointF center(F(rect.left + rect.right) / 2, F(rect.top + rect.bottom) / 2);
        Gdiplus::SolidBrush glow(Plus(l.statusColor, 0x40));
        graphics.FillEllipse(&glow, center.X - F(10.0f), center.Y - F(10.0f), F(20.0f), F(20.0f));
        graphics.FillEllipse(&solid, center.X - F(5.5f), center.Y - F(5.5f), F(11.0f), F(11.0f));
        return;
    }
    if (const HICON icon = ExecutableIcon(l.statusIcon, rect.right - rect.left))
        PaintIcon(graphics, icon, rect);
    else
        LetterTile(graphics, rect);
    // The state as a dot on the icon's corner.
    const Gdiplus::PointF dot(F(rect.right) - F(3.0f), F(rect.bottom) - F(3.0f));
    Gdiplus::SolidBrush ring(Plus(theme::kCard));
    graphics.FillEllipse(&ring, dot.X - F(7.5f), dot.Y - F(7.5f), F(15.0f), F(15.0f));
    graphics.FillEllipse(&solid, dot.X - F(4.5f), dot.Y - F(4.5f), F(9.0f), F(9.0f));
}

void Switch(Gdiplus::Graphics& graphics, const RECT& rect, bool on, bool hovered)
{
    const unsigned track = on ? (hovered ? theme::kAccentHover : theme::kAccent) : (hovered ? theme::kBorderStrong : theme::kBorder);
    const float height = F(rect.bottom - rect.top);
    FillRounded(graphics, rect, height / 2, Plus(track), Plus(track));
    const float knob = height - F(6.0f);
    const float x = on ? F(rect.right) - F(3.0f) - knob : F(rect.left) + F(3.0f);
    Gdiplus::SolidBrush brush(Plus(on ? 0xFFFFFF : theme::kDim));
    graphics.FillEllipse(&brush, x, F(rect.top) + F(3.0f), knob, knob);
}

void RemoveIcon(Gdiplus::Graphics& graphics, const RECT& rect, bool hovered)
{
    const Gdiplus::PointF center(F(rect.left + rect.right) / 2, F(rect.top + rect.bottom) / 2);
    const float r = F(rect.right - rect.left) / 2;
    if (hovered)
    {
        Gdiplus::SolidBrush background(Plus(theme::kError, 0x24));
        graphics.FillEllipse(&background, center.X - r, center.Y - r, r * 2, r * 2);
    }
    Gdiplus::Pen pen(Plus(hovered ? theme::kError : theme::kDim), 1.6f * l.scale);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float s = F(4.5f);
    graphics.DrawLine(&pen, center.X - s, center.Y - s, center.X + s, center.Y + s);
    graphics.DrawLine(&pen, center.X + s, center.Y - s, center.X - s, center.Y + s);
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

// The logo's ring as a strip along the top, like Setup. Drawn last so content scrolls under it.
void Strip(HDC dc, int width)
{
    Gdiplus::Graphics graphics(dc);
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
}

void PaintText(HDC dc, HFONT font, unsigned color, const std::wstring& text, RECT rect, UINT format = DT_WORDBREAK)
{
    SelectObject(dc, font);
    SetTextColor(dc, Gdi(color));
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
}

void Paint(HDC output)
{
    RECT client{};
    GetClientRect(g.launcher, &client);
    const int width = client.right;
    const int height = client.bottom;
    l.scroll = std::clamp(l.scroll, 0, std::max(0, l.height - height));
    const HDC dc = CreateCompatibleDC(output);
    const HBITMAP bitmap = CreateCompatibleBitmap(output, width, height);
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

        if (l.logo)
            graphics.DrawImage(l.logo.get(), F(l.logoRect.left), F(l.logoRect.top), F(l.logoRect.right - l.logoRect.left),
                               F(l.logoRect.bottom - l.logoRect.top));

        FillRounded(graphics, l.statusCard, F(10.0f), Plus(theme::kCard), Plus(theme::kBorder));
        StatusIcon(graphics);

        if (!l.update.version.empty())
            FillRounded(graphics, l.updateCard, F(10.0f), Plus(theme::kAccent, 0x22), Plus(theme::kAccent, 0x90));

        FillRounded(graphics, l.list, F(10.0f), Plus(theme::kCard), Plus(theme::kBorder));
        Gdiplus::Pen line(Plus(theme::kBorder), 1.0f);
        for (size_t i = 1; i < l.entries.size(); ++i)
        {
            const int top = l.entries[i].rect.top;
            graphics.DrawLine(&line, static_cast<int>(l.list.left) + P(14), top, static_cast<int>(l.list.right) - P(14), top);
        }

        for (size_t i = 0; i < l.targets.size(); ++i)
        {
            const Target& target = l.targets[i];
            const bool hovered = l.hovered == static_cast<int>(i);
            switch (target.look)
            {
            case Look::Button:
                FillRounded(graphics, target.rect, F(8.0f), Plus(hovered ? theme::kBorder : theme::kCardHover),
                            Plus(hovered ? theme::kBorderStrong : theme::kBorder));
                break;
            case Look::PrimaryButton:
            {
                const unsigned color = hovered ? theme::kAccentHover : theme::kAccent;
                FillRounded(graphics, target.rect, F(8.0f), Plus(color), Plus(color));
                break;
            }
            case Look::Switch:
                Switch(graphics, target.rect, target.on, hovered);
                break;
            case Look::Remove:
                RemoveIcon(graphics, target.rect, hovered);
                break;
            case Look::Row:
                if (hovered)
                    FillRounded(graphics, Inset(target.rect, P(4)), F(8.0f), Plus(theme::kCardHover), Plus(theme::kCardHover));
                break;
            case Look::Link:
                break;
            }
        }

        for (const Entry& entry : l.entries)
        {
            if (const HICON icon = ExecutableIcon(entry.icon, entry.iconRect.right - entry.iconRect.left))
                PaintIcon(graphics, icon, entry.iconRect);
            else
                LetterTile(graphics, entry.iconRect);
            if (!entry.enabled)
            {
                Gdiplus::SolidBrush fade(Plus(theme::kCard, 0xA0));
                graphics.FillRectangle(&fade, F(entry.iconRect.left), F(entry.iconRect.top), F(entry.iconRect.right - entry.iconRect.left),
                                       F(entry.iconRect.bottom - entry.iconRect.top));
            }
            if (entry.running)
                FillRounded(graphics, entry.badge, F(entry.badge.bottom - entry.badge.top) / 2, Plus(theme::kSuccess, 0x26),
                            Plus(theme::kSuccess, 0x26));
        }

        for (const Row& row : l.rows)
            Icon(graphics, Gdiplus::PointF(F(row.rect.left) - F(18.0f), F(row.rect.top) + F(9.0f)), row.level);

        graphics.DrawLine(&line, 0, l.footer, width, l.footer);
    }

    SetBkMode(dc, TRANSPARENT);
    PaintText(dc, l.title, theme::kText, L"Unishade",
             { l.logoRect.right + P(14), l.logoRect.top + P(1), width, l.logoRect.top + P(26) }, DT_SINGLELINE);
    PaintText(dc, l.body, theme::kDim, L"Version " UNISHADE_VERSION,
             { l.logoRect.right + P(14), l.logoRect.top + P(26), width, l.logoRect.bottom }, DT_SINGLELINE);

    if (!l.statusName.empty() && !ExecutableIcon(l.statusIcon, l.statusIconRect.right - l.statusIconRect.left))
        PaintText(dc, l.title, theme::kText, Initial(l.statusName), l.statusIconRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    PaintText(dc, l.semibold, theme::kText, l.statusTitle, l.statusTitleRect);
    PaintText(dc, l.body, theme::kDim, l.statusDetail, l.statusDetailRect);
    if (!l.update.version.empty())
        PaintText(dc, l.body, theme::kText, L"Unishade " + l.update.version + L" is available.", l.updateText,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    PaintText(dc, l.semibold, theme::kText, l.sectionTitle, l.sectionTitleRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    PaintText(dc, l.body, theme::kDim, l.sectionDetail, l.sectionDetailRect);
    for (const Entry& entry : l.entries)
    {
        if (!ExecutableIcon(entry.icon, entry.iconRect.right - entry.iconRect.left))
            PaintText(dc, l.semibold, entry.enabled ? theme::kText : theme::kDim, Initial(entry.name), entry.iconRect,
                     DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        PaintText(dc, l.strong, entry.enabled ? theme::kText : theme::kDim, entry.name, entry.nameRect,
                 DT_SINGLELINE | (entry.detail.empty() ? DT_VCENTER : DT_BOTTOM) | DT_END_ELLIPSIS);
        PaintText(dc, l.note, theme::kDim, entry.detail, entry.detailRect, DT_SINGLELINE | DT_PATH_ELLIPSIS);
        if (entry.running)
            PaintText(dc, l.note, theme::kSuccess, L"Running", entry.badge, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    if (l.entries.empty())
        PaintText(dc, l.body, theme::kDim, l.empty, Inset(l.list, P(18)), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (!l.rows.empty())
        PaintText(dc, l.semibold, theme::kText, L"Setup", l.setupTitle, DT_SINGLELINE | DT_VCENTER);
    for (const Row& row : l.rows)
        PaintText(dc, l.body, theme::kText, row.text, row.rect);

    for (size_t i = 0; i < l.targets.size(); ++i)
    {
        const Target& target = l.targets[i];
        const bool hovered = l.hovered == static_cast<int>(i);
        switch (target.look)
        {
        case Look::Link:
            if (target.action == Action::Download)
                PaintText(dc, l.semibold, hovered ? theme::kText : theme::kAccentHover, target.label, target.rect, DT_SINGLELINE | DT_VCENTER);
            else
                PaintText(dc, l.body, hovered ? theme::kText : theme::kAccentHover, target.label, target.rect, DT_SINGLELINE);
            break;
        case Look::Button:
            PaintText(dc, l.body, theme::kText, target.label, target.rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        case Look::PrimaryButton:
            PaintText(dc, l.strong, 0xFFFFFF, target.label, target.rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        default:
            break;
        }
    }

    Strip(dc, width);
    BitBlt(output, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void OpenPicker(Picker picker)
{
    l.picker = picker;
    l.windows = ListGameWindows();
    l.windowExecutables.clear();
    for (const GameWindow& window : l.windows)
        l.windowExecutables.push_back(Executable(window.processId));
}

// Saves a changed game list. A game that was turned off or removed stops being used, unless its window was
// picked for this session.
void SaveGames(std::vector<AutoGame> games)
{
    try
    {
        SaveAutoGames(ExeDirectory() + L"games.ini", games);
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Could not save the game list: %hs", e.what());
        return;
    }
    g.autoGames = std::move(games);
    if (g.target && !g.selectedGame &&
        std::none_of(g.autoGames.begin(), g.autoGames.end(),
                     [](const AutoGame& game) { return game.enabled && MatchesExecutable(game, l.activeExecutable); }))
        StopCapture();
}

void UseWindow(size_t index)
{
    const Picker picker = std::exchange(l.picker, Picker::None);
    if (index >= l.windows.size())
        return;
    const GameWindow window = l.windows[index];
    if (picker == Picker::Add)
    {
        auto games = g.autoGames;
        try
        {
            AddAutoGame(games, window);
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Error, L"Could not add %ls: %hs", window.name.c_str(), e.what());
            return;
        }
        // Detection follows the game in front, which is the one just added.
        g.selectedGame.reset();
        SaveGames(std::move(games));
    }
    else
    {
        if (g.target)
            StopCapture();
        g.selectedGame = window;
    }
    if (GameWindowExists(window))
    {
        if (IsIconic(window.window))
            ShowWindow(window.window, SW_RESTORE);
        SetForegroundWindow(window.window);
    }
}

void Run(const Target& target)
{
    switch (target.action)
    {
    case Action::OpenLog: ShellOpen(LogPath()); return;
    case Action::Help: ShellOpen(kHelpUrl); return;
    case Action::RunSetup: ShellOpen(l.setup); return;
    case Action::Download: ShellOpen(l.update.url); return;
    case Action::AddGame: OpenPicker(Picker::Add); break;
    case Action::PickWindow: OpenPicker(Picker::Session); break;
    case Action::ClosePicker: l.picker = Picker::None; break;
    case Action::DetectAutomatically:
        g.selectedGame.reset();
        if (g.target)
            StopCapture();
        break;
    case Action::ToggleGame:
        if (target.index < g.autoGames.size())
        {
            auto games = g.autoGames;
            games[target.index].enabled = !games[target.index].enabled;
            SaveGames(std::move(games));
        }
        break;
    case Action::RemoveGame:
        if (target.index < g.autoGames.size())
        {
            auto games = g.autoGames;
            games.erase(games.begin() + static_cast<std::ptrdiff_t>(target.index));
            SaveGames(std::move(games));
        }
        break;
    case Action::UseWindow: UseWindow(target.index); break;
    }
    Refresh();
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
        const int hovered = TargetAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
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
    case WM_MOUSEWHEEL:
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int scroll = std::clamp(l.scroll - GET_WHEEL_DELTA_WPARAM(wParam) * P(60) / WHEEL_DELTA, 0,
                                      std::max(0, l.height - static_cast<int>(client.bottom)));
        if (scroll != l.scroll)
        {
            l.scroll = scroll;
            Relayout();
            UpdateHover();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && l.hovered >= 0)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        if (const int index = TargetAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) }); index >= 0)
        {
            // Copied, since running it lays the window out again.
            const Target target = l.targets[index];
            Run(target);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && l.picker != Picker::None)
        {
            l.picker = Picker::None;
            Refresh();
            return 0;
        }
        break;
    case WM_DPICHANGED:
    {
        l.scale = HIWORD(wParam) / 96.0f;
        CreateFonts();
        ClearIcons();
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
    g.launcher = CreateWindowExW(0, kLauncherClass, L"Unishade", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    winrt::check_bool(g.launcher != nullptr);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g.launcher, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = Gdi(theme::kBackground);
    DwmSetWindowAttribute(g.launcher, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    l.scale = GetDpiForWindow(g.launcher) / 96.0f;
    CreateFonts();
    Refresh();

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
    if (g.launcher && Shown() != l.shown)
        Refresh();
}

void DestroyLauncher()
{
    if (g.launcher)
        DestroyWindow(g.launcher);
    g.launcher = nullptr;
    DeleteFonts();
    ClearIcons();
    l.logo.reset();
    l.logoStream = nullptr;
    if (l.gdiplus)
        Gdiplus::GdiplusShutdown(l.gdiplus);
}
