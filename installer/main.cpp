// RobloxShadeHost Setup: installs the host with ReShade, its effects, presets and an optional add-on, and
// later changes shortcuts or uninstalls. Drawn with Dear ImGui, the UI library ReShade's own menu uses.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "install.h"
#include "resource.h"
#include "text.h"
#include "../src/hotkey.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <functional>
#include <initializer_list>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
// Layout in pixels at 100% scaling.
constexpr float kWindowWidth = 900;
constexpr float kWindowHeight = 640;
constexpr float kStrip = 3;
constexpr float kSidebar = 236;
constexpr float kFooter = 74;

constexpr ImU32 kBackground = IM_COL32(17, 18, 23, 255);
constexpr ImU32 kSidebarColor = IM_COL32(12, 13, 17, 255);
constexpr ImU32 kCard = IM_COL32(25, 26, 33, 255);
constexpr ImU32 kCardHover = IM_COL32(31, 32, 41, 255);
constexpr ImU32 kBorder = IM_COL32(40, 42, 53, 255);
constexpr ImU32 kBorderStrong = IM_COL32(74, 77, 94, 255);
constexpr ImU32 kText = IM_COL32(236, 236, 241, 255);
constexpr ImU32 kDim = IM_COL32(150, 152, 167, 255);
constexpr ImU32 kAccent = IM_COL32(112, 122, 255, 255);
constexpr ImU32 kAccentHover = IM_COL32(132, 141, 255, 255);
constexpr ImU32 kAccentActive = IM_COL32(95, 104, 235, 255);
constexpr ImU32 kWarning = IM_COL32(245, 192, 92, 255);
constexpr ImU32 kError = IM_COL32(255, 118, 118, 255);
constexpr ImU32 kSuccess = IM_COL32(104, 214, 148, 255);
// The ring in the logo.
constexpr ImU32 kRainbow[] = {
    IM_COL32(255, 72, 96, 255),  IM_COL32(255, 158, 54, 255), IM_COL32(248, 228, 76, 255), IM_COL32(84, 222, 122, 255),
    IM_COL32(62, 198, 255, 255), IM_COL32(84, 110, 255, 255), IM_COL32(186, 92, 255, 255),
};

constexpr wchar_t kDefaultToggleKey[] = L"Home";
constexpr wchar_t kDefaultOverlayToggleKey[] = L"Ctrl+F8";

enum class Page
{
    Welcome,
    Manage,
    Addons,
    License,
    Installing,
    Failed,
    Shortcuts,
    Finished,
    Uninstall,
    Uninstalled,
};

// Runs work on its own thread. The window calls Collect every frame to learn when it has finished.
class Task
{
public:
    ~Task() { Wait(); }

    void Start(std::function<void()> work)
    {
        Wait();
        finished = false;
        error.clear();
        cancelled = false;
        thread = std::thread([this, work = std::move(work)] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try
            {
                work();
            }
            catch (const Cancelled&)
            {
                cancelled = true;
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
            CoUninitialize();
            finished = true;
        });
    }

    bool Running() const { return thread.joinable(); }

    // Returns true once, after the work has finished. error and cancelled are valid from then on.
    bool Collect()
    {
        if (!thread.joinable() || !finished)
            return false;
        thread.join();
        return true;
    }

    void Wait()
    {
        if (thread.joinable())
            thread.join();
    }

    std::string error;
    bool cancelled = false;

private:
    std::thread thread;
    std::atomic<bool> finished = true;
};

struct Arguments
{
    bool silent = false;
    bool uninstall = false;
    bool deleteUserFiles = false;
    bool acceptLicense = false;
    bool portable = false;
    std::wstring directory;
    std::wstring components;
    std::wstring log;
    std::string error;
};

struct App
{
    Page page = Page::Welcome;
    std::optional<Installation> installation;
    char directory[1024]{};
    std::string directoryError;
    bool presets = true;
    Addon addon = Addon::None;

    Task releaseTask;
    std::atomic<bool> releaseCancel = false;
    ReShadeRelease fetchedRelease;
    std::optional<ReShadeRelease> release;
    std::string releaseError;
    bool accepted = false;

    Task installTask;
    std::unique_ptr<Progress> progress;
    std::vector<std::string> notes;
    bool closeRequested = false;

    Hotkey toggleKey;
    Hotkey overlayToggleKey;
    int capturing = -1; // which shortcut is waiting for a key press
    std::string captureError;
    bool shortcutsOnly = false;
    std::string saveError;
    bool hostRunning = false;
    bool launch = true;

    Task uninstallTask;
    bool deleteUserFiles = false;
};
App app;

struct Ui
{
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> logo;
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
    float scale = 1;
    UINT resizeWidth = 0;
    UINT resizeHeight = 0;
};
Ui ui;

float S(float value)
{
    return value * ui.scale;
}

fs::path Directory()
{
    return fs::path(Wide(app.directory)).lexically_normal();
}

fs::path DefaultDirectory()
{
    PWSTR path = nullptr;
    SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &path);
    fs::path directory = fs::path(path ? path : L"") / L"RobloxShadeHost";
    CoTaskMemFree(path);
    return directory;
}

Hotkey ParseOr(const std::wstring& text, const wchar_t* fallback)
{
    Hotkey hotkey;
    if (text.empty() || !ParseHotkey(text, hotkey))
        ParseHotkey(fallback, hotkey);
    return hotkey;
}

void LoadShortcuts()
{
    const fs::path directory = Directory();
    app.toggleKey = ParseOr(ReadShortcut(directory, L"ToggleKey", kDefaultToggleKey), kDefaultToggleKey);
    const std::wstring overlay = ReadShortcut(directory, L"OverlayToggleKey", kDefaultOverlayToggleKey);
    app.overlayToggleKey = overlay.empty() ? Hotkey{} : ParseOr(overlay, kDefaultOverlayToggleKey);
}

void StartReleaseFetch()
{
    app.releaseError.clear();
    app.releaseTask.Start([] { app.fetchedRelease = FetchReShadeRelease(app.releaseCancel); });
}

void StartInstall()
{
    InstallOptions options;
    options.directory = Directory();
    options.presets = app.presets;
    options.addon = app.addon;
    app.progress = std::make_unique<Progress>();
    app.notes.clear();
    app.installTask.Start([options, release = *app.release] { Install(options, release, *app.progress); });
    app.page = Page::Installing;
}

void StartUninstall()
{
    app.uninstallTask.Start([directory = Directory(), deleteUserFiles = app.deleteUserFiles] { Uninstall(directory, deleteUserFiles); });
}

// Returns why the folder cannot be used, or an empty string.
std::string CheckDirectory(const fs::path& directory)
{
    if (!directory.is_absolute() || !directory.has_filename())
        return "Enter a full folder path, such as C:\\Games\\RobloxShadeHost.";
    std::error_code ignored;
    const std::wstring lower = [&] {
        std::wstring text = directory.wstring();
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    }();
    if (fs::exists(directory / L"RobloxPlayerBeta.exe", ignored) || lower.find(L"\\roblox\\versions") != std::wstring::npos)
        return "That is Roblox's folder, which Roblox replaces when it updates. Pick another folder.";

    // Tries the nearest folder that exists, so nothing is created before installing.
    fs::path existing = directory;
    while (!fs::is_directory(existing, ignored) && existing.has_relative_path())
        existing = existing.parent_path();
    const fs::path probe = existing / (L"RobloxShadeHost-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return "Setup cannot write to this folder. Pick another one, such as the suggested folder.";
    CloseHandle(file);
    return {};
}

std::optional<fs::path> BrowseFolder(const fs::path& current)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::error_code ignored;
    fs::path existing = current;
    while (!existing.empty() && !fs::is_directory(existing, ignored) && existing.has_relative_path())
        existing = existing.parent_path();
    ComPtr<IShellItem> start;
    if (!existing.empty() && SUCCEEDED(SHCreateItemFromParsingName(existing.c_str(), nullptr, IID_PPV_ARGS(&start))))
        dialog->SetFolder(start.Get());

    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(ui.window)) || FAILED(dialog->GetResult(&result)) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    fs::path folder = path;
    CoTaskMemFree(path);
    // Keeps the files together instead of spreading them over the chosen folder.
    if (_wcsicmp(folder.filename().c_str(), L"RobloxShadeHost") != 0)
        folder /= L"RobloxShadeHost";
    return folder;
}

void OpenFile(const fs::path& path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Drawing helpers.

void Spacing(float height)
{
    ImGui::Dummy(ImVec2(0, S(height)));
}

void Text(const std::string& text, ImU32 color = kText, float size = 15.5f, ImFont* font = nullptr)
{
    ImGui::PushFont(font ? font : ui.regular, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Title(const std::string& text)
{
    Text(text, kText, 27, ui.semibold);
    Spacing(2);
}

void Rainbow(ImDrawList* draw, ImVec2 min, ImVec2 max)
{
    constexpr int count = IM_ARRAYSIZE(kRainbow);
    for (int i = 0; i + 1 < count; ++i)
    {
        const float left = min.x + (max.x - min.x) * i / (count - 1);
        const float right = min.x + (max.x - min.x) * (i + 1) / (count - 1);
        draw->AddRectFilledMultiColor(ImVec2(left, min.y), ImVec2(right, max.y), kRainbow[i], kRainbow[i + 1], kRainbow[i + 1], kRainbow[i]);
    }
}

void CheckBox(ImDrawList* draw, ImVec2 position, bool checked)
{
    const float size = S(20);
    if (checked)
    {
        draw->AddRectFilled(position, position + ImVec2(size, size), kAccent, S(5));
        const ImVec2 points[] = { position + ImVec2(size * 0.26f, size * 0.52f), position + ImVec2(size * 0.44f, size * 0.70f),
                                  position + ImVec2(size * 0.76f, size * 0.32f) };
        draw->AddPolyline(points, 3, IM_COL32_WHITE, S(2.2f));
    }
    else
        draw->AddRect(position, position + ImVec2(size, size), kBorderStrong, S(5), S(1.5f));
}

void Pill(const char* text, ImU32 color)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui.semibold, 12);
    const ImVec2 size = ImGui::CalcTextSize(text) + ImVec2(S(16), S(6));
    const ImVec2 position = ImGui::GetCursorScreenPos() + ImVec2(0, S(2));
    draw->AddRectFilled(position, position + size, (color & 0x00FFFFFF) | 0x2E000000, size.y / 2);
    draw->AddText(position + ImVec2(S(8), S(3)), color, text);
    ImGui::Dummy(size);
    ImGui::PopFont();
}

enum class CardKind
{
    Static,
    Toggle, // with a checkbox
    Action, // with an arrow
};

// A card with a title, an optional tag and a description. Returns true when a toggle or action card is clicked.
bool Card(const char* id, const char* title, const char* tag, ImU32 tagColor, const std::string& description, CardKind kind, bool checked = false)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float padding = S(16);
    const float indent = kind == CardKind::Toggle ? S(34) : 0;
    const float arrow = kind == CardKind::Action ? S(30) : 0;
    const bool interactive = kind != CardKind::Static;
    checked &= kind == CardKind::Toggle;

    // The content goes on top, so the background can be sized to it afterwards.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(start + ImVec2(padding + indent, padding));
    ImGui::BeginGroup();
    ImGui::PushFont(ui.semibold, 16.5f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (tag)
    {
        ImGui::SameLine(0, S(10));
        Pill(tag, tagColor);
    }
    ImGui::PushFont(ui.regular, 14.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(start.x + width - padding - arrow - ImGui::GetWindowPos().x + ImGui::GetScrollX());
    ImGui::TextUnformatted(description.c_str(), description.c_str() + description.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    const ImVec2 size(width, ImGui::GetItemRectMax().y - start.y + padding);

    ImGui::SetCursorScreenPos(start);
    bool clicked = false;
    bool hovered = false;
    if (interactive)
    {
        clicked = ImGui::InvisibleButton(id, size);
        hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    else
        ImGui::Dummy(size);

    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(10));
    draw->AddRect(start, start + size, checked ? kAccent : kBorder, S(10), checked ? S(1.5f) : S(1));
    if (kind == CardKind::Toggle)
        CheckBox(draw, start + ImVec2(padding, padding + S(1)), checked);
    if (kind == CardKind::Action)
    {
        const ImVec2 tip(start.x + width - padding - S(6), start.y + size.y / 2);
        const ImVec2 points[] = { tip + ImVec2(-S(5), -S(6)), tip, tip + ImVec2(-S(5), S(6)) };
        draw->AddPolyline(points, 3, hovered ? kText : kDim, S(1.8f));
    }
    draw->ChannelsMerge();
    return clicked;
}

// A checkbox drawn like the ones on cards.
void CheckLine(const char* label, bool& value)
{
    ImGui::PushFont(ui.regular, 15.5f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(S(30) + ImGui::CalcTextSize(label).x, S(24));
    if (ImGui::InvisibleButton(label, size))
        value = !value;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    CheckBox(draw, start + ImVec2(0, S(2)), value);
    draw->AddText(start + ImVec2(S(30), (size.y - ImGui::GetFontSize()) / 2), hovered ? IM_COL32_WHITE : kText, label);
    ImGui::PopFont();
}

bool Button(const char* label, ImVec2 size, bool primary, bool enabled = true)
{
    ImGui::PushFont(ui.semibold, 15);
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? kAccent : kCard);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? kAccentHover : kCardHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? kAccentActive : kBorder);
    ImGui::PushStyleColor(ImGuiCol_Border, primary ? kAccent : kBorder);
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? IM_COL32_WHITE : kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    if (enabled && ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    return clicked;
}

// Footer buttons are numbered from the right.
bool FooterButton(int slot, const char* label, bool primary, bool enabled = true)
{
    const ImVec2 size(S(124), S(38));
    const ImVec2 window = ImGui::GetWindowSize();
    ImGui::SetCursorPos(ImVec2(window.x - S(28) - (slot + 1) * size.x - slot * S(10), window.y - (S(kFooter) + size.y) / 2));
    return Button(label, size, primary, enabled);
}

bool Link(const char* label, ImU32 color = kDim, float size = 14)
{
    ImGui::PushFont(ui.regular, size);
    ImGui::PushStyleColor(ImGuiCol_TextLink, color);
    const bool clicked = ImGui::TextLink(label);
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    return clicked;
}

void Spinner(float radius)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(radius, radius);
    const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(center, radius - S(2), start, start + 4.2f, 32);
    draw->PathStroke(kAccent, S(3));
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

void ProgressLine(float fraction)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end = start + ImVec2(ImGui::GetContentRegionAvail().x, S(6));
    draw->AddRectFilled(start, end, kCard, S(3));
    draw->PushClipRect(start, ImVec2(start.x + (end.x - start.x) * std::clamp(fraction, 0.0f, 1.0f), end.y), true);
    Rainbow(draw, start, end);
    draw->PopClipRect();
    ImGui::Dummy(end - start);
}

// A shortcut drawn like a key, followed by what it does.
void KeyLine(const Hotkey& hotkey, const std::string& description)
{
    const std::string key = Utf8(FormatHotkey(hotkey));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui.semibold, 14.5f);
    const ImVec2 text = ImGui::CalcTextSize(key.c_str());
    const ImVec2 size(std::max(text.x + S(20), S(92)), text.y + S(10));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(position, position + size, kCard, S(6));
    draw->AddRect(position, position + size, kBorderStrong, S(6), S(1));
    draw->AddText(position + ImVec2((size.x - text.x) / 2, S(5)), kText, key.c_str());
    ImGui::Dummy(size);
    ImGui::PopFont();
    ImGui::SameLine(0, S(14));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
    Text(description, kText, 15);
}

// The key a shortcut uses, and a button that waits for new keys when clicked.
void ShortcutRow(int index, const char* title, const char* description, Hotkey& hotkey, bool clearable)
{
    const float buttonWidth = S(190);
    const float right = ImGui::GetContentRegionAvail().x;
    const float top = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + right - buttonWidth - S(56));
    ImGui::PushFont(ui.semibold, 16.5f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushFont(ui.regular, 14.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(description);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float bottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + right - buttonWidth - S(40), top + S(4)));
    const bool capturing = app.capturing == index;
    const std::string label = capturing ? "Press keys..." : hotkey.key ? Utf8(FormatHotkey(hotkey)) : "Not set";
    ImGui::PushID(index);
    ImGui::PushStyleColor(ImGuiCol_Border, capturing ? kAccent : kBorderStrong);
    ImGui::PushStyleColor(ImGuiCol_Text, capturing ? kAccentHover : hotkey.key ? kText : kDim);
    if (Button(label.c_str(), ImVec2(buttonWidth, S(40)), false))
    {
        app.capturing = capturing ? -1 : index;
        app.captureError.clear();
    }
    ImGui::PopStyleColor(2);
    if (clearable)
    {
        ImGui::SameLine(0, S(6));
        ImGui::BeginDisabled(!hotkey.key);
        if (Button("x", ImVec2(S(34), S(40)), false))
        {
            hotkey = {};
            app.capturing = -1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetItemTooltip("Leave unassigned");
    }
    ImGui::PopID();
    ImGui::SetCursorPosY(std::max(bottom, top + S(48)) + S(14));
}

void Sidebar(std::initializer_list<const char*> steps, int current)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos() + ImVec2(0, S(kStrip));
    const float height = ImGui::GetWindowSize().y;
    draw->AddRectFilled(origin, ImVec2(origin.x + S(kSidebar), origin.y + height), kSidebarColor);
    draw->AddLine(ImVec2(origin.x + S(kSidebar), origin.y), ImVec2(origin.x + S(kSidebar), origin.y + height), kBorder);

    const float x = origin.x + S(30);
    float y = origin.y + S(30);
    if (ui.logo)
        draw->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(ui.logo.Get())), ImVec2(x - S(8), y), ImVec2(x - S(8) + S(80), y + S(80)));
    y += S(92);
    draw->AddText(ui.semibold, S(18), ImVec2(x, y), kText, "RobloxShadeHost");
    y += S(25);
    draw->AddText(ui.regular, S(13.5f), ImVec2(x, y), kDim, "Setup " ROBLOX_SHADE_HOST_VERSION);
    y += S(48);

    int index = 0;
    for (const char* step : steps)
    {
        const ImVec2 dot(x + S(7), y + S(11));
        if (index + 1 < static_cast<int>(steps.size()))
            draw->AddLine(dot + ImVec2(0, S(9)), dot + ImVec2(0, S(27)), kBorder, S(1.5f));
        if (index < current)
        {
            draw->AddCircleFilled(dot, S(7), IM_COL32(104, 214, 148, 40));
            const ImVec2 check[] = { dot + ImVec2(-S(3.2f), 0), dot + ImVec2(-S(0.8f), S(2.5f)), dot + ImVec2(S(3.5f), -S(2.5f)) };
            draw->AddPolyline(check, 3, kSuccess, S(1.8f));
        }
        else if (index == current)
        {
            draw->AddCircleFilled(dot, S(9), IM_COL32(112, 122, 255, 60));
            draw->AddCircleFilled(dot, S(5), kAccent);
        }
        else
            draw->AddCircle(dot, S(5), kBorderStrong, 0, S(1.5f));
        draw->AddText(index == current ? ui.semibold : ui.regular, S(15), ImVec2(x + S(24), y + S(1)), index == current ? kText : kDim, step);
        y += S(36);
        ++index;
    }

    ImGui::SetCursorPos(ImVec2(S(30), height - S(46)));
    if (Link("Credits and licenses", kDim, 13.5f))
        ImGui::OpenPopup("Credits");
}

void CreditsPopup()
{
    const ImVec2 window = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(ImGui::GetWindowPos() + window / 2, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(640), S(470)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(20)));
    if (ImGui::BeginPopupModal("Credits", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
    {
        Text("Credits and licenses", kText, 20, ui.semibold);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
        ImGui::BeginChild("text", ImVec2(0, ImGui::GetContentRegionAvail().y - S(36) - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_Borders);
        ImGui::PopStyleVar();
        const std::string_view credits = Resource(IDR_CREDITS);
        Text(std::string(credits), kDim, 14);
        ImGui::EndChild();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(24) - S(124));
        if (Button("Close", ImVec2(S(124), S(36)), false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// Pages. Each draws its content, then its footer buttons.

void WelcomePage()
{
    Title("Install RobloxShadeHost");
    Text("ReShade effects for Roblox. RobloxShadeHost runs alongside the game and never modifies Roblox or its files.");
    Text("Your frame rate will be lower while it runs.", kDim, 15);
    Spacing(18);
    Text("Install folder", kText, 15, ui.semibold);
    ImGui::PushFont(ui.regular, 15);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(124) - S(10));
    if (ImGui::InputText("##directory", app.directory, sizeof(app.directory), ImGuiInputTextFlags_ElideLeft))
        app.directoryError.clear();
    ImGui::PopFont();
    ImGui::SameLine(0, S(10));
    if (Button("Browse", ImVec2(S(124), 0), false))
        if (auto folder = BrowseFolder(Directory()))
        {
            strncpy_s(app.directory, Utf8(folder->wstring()).c_str(), _TRUNCATE);
            app.directoryError.clear();
        }
    if (!app.directoryError.empty())
        Text(app.directoryError, kError, 14.5f);
}

void ManagePage()
{
    Title("RobloxShadeHost is installed");
    std::string version = app.installation && !app.installation->version.empty() ? "Version " + app.installation->version : "RobloxShadeHost";
    Text(version + " is installed in " + app.directory + ".", kDim, 15);
    if (app.installation && app.installation->version != ROBLOX_SHADE_HOST_VERSION)
        Text("This setup installs version " ROBLOX_SHADE_HOST_VERSION ".", kDim, 15);
    Spacing(14);
    if (Card("update", "Update or change add-ons", nullptr, 0,
             "Get the newest ReShade and effects, or add or remove depth estimation and DLSS5. Your settings and presets stay.", CardKind::Action))
    {
        app.addon = InstalledAddon(Directory());
        app.page = Page::Addons;
    }
    if (Card("shortcuts", "Change shortcuts", nullptr, 0, "Pick the keys that open ReShade and turn the overlay off.", CardKind::Action))
    {
        LoadShortcuts();
        app.shortcutsOnly = true;
        app.page = Page::Shortcuts;
    }
    if (Card("uninstall", "Uninstall", nullptr, 0, "Remove RobloxShadeHost, ReShade and the effects from this PC.", CardKind::Action))
        app.page = Page::Uninstall;
}

void AddonsPage()
{
    Title("Choose what to install");
    Spacing(6);
    Card("reshade", "ReShade and effects", "Included", kDim, "ReShade from reshade.me and every effect package on ReShade's official list.", CardKind::Static);
    if (Card("presets", "Presets", nullptr, 0, "Ready-made looks for Roblox. Pick one from the list at the top of the ReShade menu.", CardKind::Toggle, app.presets))
        app.presets = !app.presets;
    Spacing(8);
    Text("Optional add-ons", kText, 15, ui.semibold);
    Text("Pick one or none. They do not work together.", kDim, 14.5f);
    if (Card("depth", "Depth estimation", "Experimental", kWarning,
             "Makes effects that need depth work, like ambient occlusion, depth of field and fog. An AI model estimates depth from the "
             "picture on your GPU, which lowers your frame rate. 85 MB download.",
             CardKind::Toggle, app.addon == Addon::Depth))
        app.addon = app.addon == Addon::Depth ? Addon::None : Addon::Depth;
    if (Card("dlss5", "DLSS5", "NVIDIA RTX", kSuccess,
             "NVIDIA's DLSS5 through RenoDX's ReShade add-on. Needs an NVIDIA RTX graphics card. 168 MB download.",
             CardKind::Toggle, app.addon == Addon::DLSS5))
        app.addon = app.addon == Addon::DLSS5 ? Addon::None : Addon::DLSS5;
}

void LicensePage()
{
    Title("ReShade license");
    if (app.release)
    {
        Text("Setup downloads ReShade " + app.release->version + " from reshade.me. Read its license to continue.", kDim, 15);
        Spacing(4);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
        ImGui::BeginChild("license", ImVec2(0, ImGui::GetContentRegionAvail().y - S(48)), ImGuiChildFlags_Borders);
        Text(app.release->license, kDim, 14);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        Spacing(4);
        CheckLine("I accept the ReShade license", app.accepted);
    }
    else if (app.releaseTask.Running())
    {
        Spacing(10);
        Spinner(S(14));
        ImGui::SameLine(0, S(12));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
        Text("Finding the newest ReShade...", kDim, 15);
    }
    else
    {
        Text(app.releaseError, kError, 15);
        Spacing(4);
        if (Button("Try again", ImVec2(S(124), S(36)), false))
            StartReleaseFetch();
    }
}

void InstallingPage()
{
    const Progress::State state = app.progress->Read();
    Title(app.progress->cancel ? "Cancelling..." : "Installing");
    Spacing(4);
    Text(state.status.empty() ? "Starting..." : state.status, kText, 15.5f, ui.semibold);
    ProgressLine(state.fraction);
    Text(state.detail.empty() ? " " : state.detail, kDim, 14);
    Spacing(8);
    ImGui::BeginChild("history", ImVec2(0, 0));
    for (size_t i = 0; i + 1 < state.history.size(); ++i)
        Text(state.history[i], kDim, 14);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void FailedPage()
{
    const bool cancelled = app.installTask.cancelled;
    Title(cancelled ? "Installation cancelled" : "Setup could not finish");
    if (!cancelled)
        Text(app.installTask.error, kError, 15);
    Spacing(6);
    Text("Go back to try again, or leave out the part that failed.", kDim, 15);
    Spacing(6);
    if (Link("Open the setup log", kAccentHover, 15))
        OpenFile(SetupLogPath());
}

void ShortcutsPage()
{
    Title("Shortcuts");
    Text("Click a shortcut, then press the keys you want.", kDim, 15);
    Spacing(16);
    ShortcutRow(0, "Open ReShade", "Opens the ReShade menu over Roblox and gives it your mouse and keyboard. Press it again to go back to playing.",
                app.toggleKey, false);
    ShortcutRow(1, "Turn the overlay off and on", "Shows Roblox without effects and stops capturing it until you press it again.",
                app.overlayToggleKey, true);

    const auto bare = [](const Hotkey& hotkey) { return hotkey.key && !(hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)); };
    const bool typing = app.toggleKey.key == VK_SPACE || app.toggleKey.key == VK_TAB || (app.toggleKey.key >= '0' && app.toggleKey.key <= 'Z');
    if (!app.captureError.empty())
        Text(app.captureError, kError, 14.5f);
    else if (app.overlayToggleKey.key == app.toggleKey.key && app.overlayToggleKey.modifiers == app.toggleKey.modifiers)
        Text("Pick two different shortcuts.", kError, 14.5f);
    else
    {
        if (bare(app.toggleKey) && typing)
            Text("Roblox will not receive " + Utf8(FormatHotkey(app.toggleKey)) + " while RobloxShadeHost runs.", kWarning, 14.5f);
        if (bare(app.overlayToggleKey))
            Text("Other programs will not receive " + Utf8(FormatHotkey(app.overlayToggleKey)) + " while RobloxShadeHost runs.", kWarning, 14.5f);
    }
    if (!app.saveError.empty())
        Text(app.saveError, kError, 14.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6));
    if (Link("Reset to defaults", kDim, 14))
    {
        ParseHotkey(kDefaultToggleKey, app.toggleKey);
        ParseHotkey(kDefaultOverlayToggleKey, app.overlayToggleKey);
        app.capturing = -1;
        app.captureError.clear();
    }
}

void FinishedPage()
{
    if (app.shortcutsOnly)
    {
        Title("Shortcuts saved");
        Text(app.hostRunning ? "Restart RobloxShadeHost to use them." : "RobloxShadeHost uses them the next time it starts.", kDim, 15);
    }
    else
    {
        Title("RobloxShadeHost is ready");
        Text("Open a Roblox experience and start RobloxShadeHost from the Start menu, in either order.", kDim, 15);
    }
    Spacing(14);
    KeyLine(app.toggleKey, "opens ReShade over Roblox. Press it again to go back to playing.");
    if (app.overlayToggleKey.key)
        KeyLine(app.overlayToggleKey, "turns the overlay off and on.");
    if (!app.shortcutsOnly && app.presets)
    {
        Spacing(4);
        Text("Pick a preset from the list at the top of the ReShade menu.", kText, 15);
    }
    for (const auto& note : app.notes)
    {
        Spacing(4);
        Text(note, kWarning, 14.5f);
    }
    if (!app.hostRunning)
    {
        Spacing(14);
        CheckLine("Start RobloxShadeHost now", app.launch);
    }
}

void UninstallPage()
{
    Title("Uninstall RobloxShadeHost");
    Text("Removes RobloxShadeHost, ReShade and the effects from " + std::string(app.directory) + ", and its Start menu shortcuts.", kDim, 15);
    Spacing(10);
    CheckLine("Also delete my presets, ReShade settings and shortcuts", app.deleteUserFiles);
    if (app.uninstallTask.Running())
    {
        Spacing(10);
        Spinner(S(12));
    }
}

void UninstalledPage()
{
    if (app.uninstallTask.error.empty())
    {
        Title("RobloxShadeHost was uninstalled");
        if (!app.deleteUserFiles)
            Text("Your presets, ReShade settings and shortcuts are still in " + std::string(app.directory) + ".", kDim, 15);
    }
    else
    {
        Title("Uninstall could not finish");
        Text(app.uninstallTask.error, kError, 15);
    }
}

void CollectTasks()
{
    if (app.releaseTask.Collect())
    {
        if (app.releaseTask.error.empty())
            app.release = app.fetchedRelease;
        else
            app.releaseError = app.releaseTask.error;
    }
    if (app.installTask.Collect())
    {
        if (!app.installTask.error.empty() || app.installTask.cancelled)
            app.page = Page::Failed;
        else
        {
            app.notes = app.progress->Read().notes;
            app.installation = FindInstallation();
            LoadShortcuts();
            app.page = Page::Shortcuts;
        }
        if (app.closeRequested)
            DestroyWindow(ui.window);
    }
    if (app.uninstallTask.Collect())
        app.page = Page::Uninstalled;
}

void DrawUi()
{
    CollectTasks();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("Setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    Rainbow(draw, origin, origin + ImVec2(size.x, S(kStrip)));

    const Page page = app.page;
    if (page == Page::Uninstall || page == Page::Uninstalled)
        Sidebar({ "Uninstall", "Done" }, page == Page::Uninstall ? 0 : 1);
    else if (app.shortcutsOnly && (page == Page::Shortcuts || page == Page::Finished))
        Sidebar({ "Shortcuts", "Done" }, page == Page::Shortcuts ? 0 : 1);
    else
    {
        int step = 0;
        switch (page)
        {
        case Page::Addons: step = 1; break;
        case Page::License: step = 2; break;
        case Page::Installing:
        case Page::Failed: step = 3; break;
        case Page::Shortcuts: step = 4; break;
        case Page::Finished: step = 5; break;
        default: break;
        }
        Sidebar({ "Welcome", "Add-ons", "License", "Install", "Shortcuts", "Done" }, step);
    }

    // Content, above a footer separated by a line.
    const float left = S(kSidebar) + 1;
    draw->AddLine(origin + ImVec2(left, size.y - S(kFooter)), origin + ImVec2(size.x, size.y - S(kFooter)), kBorder);
    ImGui::SetCursorPos(ImVec2(left, S(kStrip)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(44), S(34)));
    ImGui::BeginChild("content", ImVec2(size.x - left, size.y - S(kStrip) - S(kFooter)), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (page)
    {
    case Page::Welcome: WelcomePage(); break;
    case Page::Manage: ManagePage(); break;
    case Page::Addons: AddonsPage(); break;
    case Page::License: LicensePage(); break;
    case Page::Installing: InstallingPage(); break;
    case Page::Failed: FailedPage(); break;
    case Page::Shortcuts: ShortcutsPage(); break;
    case Page::Finished: FinishedPage(); break;
    case Page::Uninstall: UninstallPage(); break;
    case Page::Uninstalled: UninstalledPage(); break;
    }
    ImGui::EndChild();

    switch (page)
    {
    case Page::Welcome:
        if (FooterButton(0, "Next", true))
        {
            app.directoryError = CheckDirectory(Directory());
            if (app.directoryError.empty())
                app.page = Page::Addons;
        }
        break;
    case Page::Manage:
        if (FooterButton(0, "Close", false))
            DestroyWindow(ui.window);
        break;
    case Page::Addons:
        if (FooterButton(0, "Next", true))
            app.page = Page::License;
        if (FooterButton(1, "Back", false))
            app.page = app.installation ? Page::Manage : Page::Welcome;
        break;
    case Page::License:
        if (FooterButton(0, "Install", true, app.release && app.accepted))
            StartInstall();
        if (FooterButton(1, "Back", false))
            app.page = Page::Addons;
        break;
    case Page::Installing:
        if (FooterButton(0, "Cancel", false, !app.progress->cancel))
            app.progress->cancel = true;
        break;
    case Page::Failed:
        if (FooterButton(0, "Back", true))
            app.page = Page::Addons;
        if (FooterButton(1, "Close", false))
            DestroyWindow(ui.window);
        break;
    case Page::Shortcuts:
    {
        const bool distinct = app.overlayToggleKey.key != app.toggleKey.key || app.overlayToggleKey.modifiers != app.toggleKey.modifiers;
        if (FooterButton(0, "Save", true, distinct && app.capturing < 0))
        {
            try
            {
                WriteShortcuts(Directory(), FormatHotkey(app.toggleKey), FormatHotkey(app.overlayToggleKey));
                app.saveError.clear();
                app.hostRunning = HostRunning(Directory());
                app.page = Page::Finished;
            }
            catch (const std::exception& e)
            {
                app.saveError = e.what();
            }
        }
        if (app.shortcutsOnly && FooterButton(1, "Back", false))
        {
            app.shortcutsOnly = false;
            app.capturing = -1;
            app.page = Page::Manage;
        }
        break;
    }
    case Page::Finished:
        if (FooterButton(0, "Finish", true))
        {
            if (app.launch && !app.hostRunning)
                LaunchHost(Directory());
            DestroyWindow(ui.window);
        }
        break;
    case Page::Uninstall:
        if (FooterButton(0, "Uninstall", true, !app.uninstallTask.Running()))
            StartUninstall();
        if (FooterButton(1, app.installation ? "Back" : "Cancel", false, !app.uninstallTask.Running()))
        {
            if (app.installation)
                app.page = Page::Manage;
            else
                DestroyWindow(ui.window);
        }
        break;
    case Page::Uninstalled:
        if (FooterButton(0, "Close", true))
            DestroyWindow(ui.window);
        break;
    }

    // Clicking anywhere else stops waiting for a shortcut.
    if (app.capturing >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        app.capturing = -1;

    CreditsPopup();
    ImGui::End();
}

void CaptureKey(WPARAM key, LPARAM lParam)
{
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN)
        return;
    Hotkey hotkey;
    if (GetKeyState(VK_CONTROL) < 0)
        hotkey.modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0)
        hotkey.modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0)
        hotkey.modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
        hotkey.modifiers |= MOD_WIN;
    if (key == VK_ESCAPE && hotkey.modifiers == MOD_NOREPEAT)
    {
        app.capturing = -1;
        return;
    }
    hotkey.key = static_cast<UINT>(key);
    if (FormatHotkey(hotkey).empty())
    {
        wchar_t name[64]{};
        GetKeyNameTextW(static_cast<LONG>(lParam), name, static_cast<int>(std::size(name)));
        app.captureError = (name[0] ? Utf8(name) : std::string("That key")) +
                           " cannot be used. Use a letter, number, F key other than F12, Home, End, Insert, Delete, Page Up, Page Down, "
                           "Pause or Scroll Lock, with or without Ctrl, Alt, Shift or Win.";
        return;
    }
    (app.capturing == 0 ? app.toggleKey : app.overlayToggleKey) = hotkey;
    app.capturing = -1;
    app.captureError.clear();
}

ComPtr<ID3D11ShaderResourceView> LoadLogo(UINT size)
{
    const std::string_view png = Resource(IDR_LOGO);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> premultiplied;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> straight;
    std::vector<BYTE> pixels(size * size * 4);
    // Scaling with premultiplied alpha keeps the transparent edge from darkening.
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&premultiplied)) ||
        FAILED(premultiplied->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(premultiplied.Get(), size, size, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(&straight)) ||
        FAILED(straight->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(straight->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{ pixels.data(), size * 4, 0 };
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(ui.device->CreateTexture2D(&desc, &data, &texture)) || FAILED(ui.device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
        return nullptr;
    return view;
}

void ApplyScale(float scale)
{
    ui.scale = scale;
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0;
    style.WindowRounding = 0;
    style.PopupRounding = 12;
    style.PopupBorderSize = 1;
    style.ChildRounding = 8;
    style.ChildBorderSize = 1;
    style.FramePadding = ImVec2(12, 9);
    style.FrameRounding = 7;
    style.FrameBorderSize = 1;
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(10, 6);
    style.ScrollbarSize = 10;
    style.ScrollbarRounding = 5;
    style.GrabRounding = 5;
    style.FontSizeBase = 15.5f;

    const auto color = [&](ImGuiCol index, ImU32 value) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(value); };
    color(ImGuiCol_Text, kText);
    color(ImGuiCol_TextDisabled, kDim);
    color(ImGuiCol_WindowBg, kBackground);
    color(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_PopupBg, IM_COL32(22, 23, 29, 255));
    color(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 150));
    color(ImGuiCol_Border, kBorder);
    color(ImGuiCol_FrameBg, kCard);
    color(ImGuiCol_FrameBgHovered, kCardHover);
    color(ImGuiCol_FrameBgActive, kCardHover);
    color(ImGuiCol_CheckMark, IM_COL32_WHITE);
    color(ImGuiCol_Button, kCard);
    color(ImGuiCol_ButtonHovered, kCardHover);
    color(ImGuiCol_ButtonActive, kBorder);
    color(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_ScrollbarGrab, kBorder);
    color(ImGuiCol_ScrollbarGrabHovered, kBorderStrong);
    color(ImGuiCol_ScrollbarGrabActive, kBorderStrong);
    color(ImGuiCol_TextSelectedBg, IM_COL32(112, 122, 255, 90));
    color(ImGuiCol_NavCursor, kAccent);
    color(ImGuiCol_TextLink, kAccentHover);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui::GetStyle() = style;

    if (ui.device)
        ui.logo = LoadLogo(static_cast<UINT>(std::lround(S(80))));
}

void CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> buffer;
    ui.swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    ui.device->CreateRenderTargetView(buffer.Get(), nullptr, &ui.target);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // While a shortcut is being picked, keys go to it rather than to the window.
    if (app.capturing >= 0)
    {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        {
            CaptureKey(wParam, lParam);
            return 0;
        }
        if (message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_CHAR || message == WM_SYSCHAR)
            return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        return 1;

    switch (message)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            ui.resizeWidth = LOWORD(lParam);
            ui.resizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_DPICHANGED:
    {
        ApplyScale(HIWORD(wParam) / 96.0f);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        // Alt alone would otherwise open the window menu.
        if ((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        // Closing during installation cancels it first. The window closes once the download stops.
        if (app.installTask.Running())
        {
            app.progress->cancel = true;
            app.closeRequested = true;
            return 0;
        }
        if (app.uninstallTask.Running())
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CreateDeviceAndSwapchain()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    // WARP draws on the CPU, for machines without a usable GPU driver.
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &ui.device, nullptr, &ui.context)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &ui.device, nullptr, &ui.context)))
        return false;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(ui.device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(ui.device.Get(), ui.window, &desc, nullptr, nullptr, &ui.swapchain)))
        return false;
    factory->MakeWindowAssociation(ui.window, DXGI_MWA_NO_ALT_ENTER);
    CreateRenderTarget();
    return true;
}

void AddFonts()
{
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    ImFontConfig config;
    config.Flags |= ImFontFlags_NoLoadError;
    ImGuiIO& io = ImGui::GetIO();
    ui.regular = io.Fonts->AddFontFromFileTTF((Utf8(windows) + "\\Fonts\\segoeui.ttf").c_str(), 0.0f, &config);
    ui.semibold = io.Fonts->AddFontFromFileTTF((Utf8(windows) + "\\Fonts\\seguisb.ttf").c_str(), 0.0f, &config);
    if (!ui.regular)
        ui.regular = io.Fonts->AddFontDefault();
    if (!ui.semibold)
        ui.semibold = ui.regular;
}


int RunWindow(const Arguments& arguments)
{
    app.installation = FindInstallation();
    const fs::path directory = !arguments.directory.empty() ? fs::absolute(arguments.directory)
                               : app.installation        ? app.installation->directory
                                                         : DefaultDirectory();
    strncpy_s(app.directory, Utf8(directory.wstring()).c_str(), _TRUNCATE);
    app.addon = InstalledAddon(directory);
    if (arguments.uninstall)
        app.page = Page::Uninstall;
    else
    {
        app.page = app.installation ? Page::Manage : Page::Welcome;
        StartReleaseFetch();
    }

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(1));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(RGB(17, 18, 23));
    windowClass.lpszClassName = L"RobloxShadeHostSetup";
    RegisterClassExW(&windowClass);
    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    ui.window = CreateWindowExW(0, windowClass.lpszClassName, L"RobloxShadeHost Setup", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                                nullptr, windowClass.hInstance, nullptr);
    if (!ui.window)
        return 1;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = RGB(12, 13, 17);
    DwmSetWindowAttribute(ui.window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    // Sizes the window for its monitor's scaling and centers it.
    const UINT dpi = GetDpiForWindow(ui.window);
    RECT frame{ 0, 0, static_cast<LONG>(std::lround(kWindowWidth * dpi / 96.0f)), static_cast<LONG>(std::lround(kWindowHeight * dpi / 96.0f)) };
    AdjustWindowRectExForDpi(&frame, kStyle, FALSE, 0, dpi);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(ui.window, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    SetWindowPos(ui.window, nullptr, (monitor.rcWork.left + monitor.rcWork.right - width) / 2, (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDeviceAndSwapchain())
    {
        MessageBoxW(ui.window, L"Setup could not start its window because DirectX 11 is unavailable.", L"RobloxShadeHost Setup", MB_ICONERROR);
        return 1;
    }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui_ImplWin32_Init(ui.window);
    ImGui_ImplDX11_Init(ui.device.Get(), ui.context.Get());
    AddFonts();
    ApplyScale(dpi / 96.0f);
    ShowWindow(ui.window, SW_SHOWNORMAL);

    // Draws a few frames after each message, so hover and click states settle, then waits for input.
    int framesLeft = 3;
    for (bool running = true; running;)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            running &= message.message != WM_QUIT;
            framesLeft = 3;
        }
        if (!running)
            break;
        if (IsIconic(ui.window))
        {
            WaitMessage();
            continue;
        }
        if (ui.resizeWidth)
        {
            ui.target.Reset();
            ui.swapchain->ResizeBuffers(0, ui.resizeWidth, ui.resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            ui.resizeWidth = ui.resizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ImGui::Render();
        const float clear[] = { 17 / 255.0f, 18 / 255.0f, 23 / 255.0f, 1.0f };
        ID3D11RenderTargetView* targets[] = { ui.target.Get() };
        ui.context->OMSetRenderTargets(1, targets, nullptr);
        ui.context->ClearRenderTargetView(ui.target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ui.swapchain->Present(1, 0);

        const bool animating = app.installTask.Running() || app.uninstallTask.Running() || app.capturing >= 0 ||
                               (app.page == Page::License && app.releaseTask.Running());
        if (!animating && --framesLeft <= 0)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
    }

    app.releaseCancel = true;
    app.releaseTask.Wait();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

int RunSilent(const Arguments& arguments)
{
    try
    {
        const auto installation = FindInstallation();
        InstallOptions options;
        options.directory = !arguments.directory.empty() ? fs::absolute(arguments.directory).lexically_normal()
                            : installation               ? installation->directory
                                                         : DefaultDirectory();
        if (arguments.uninstall)
        {
            Uninstall(options.directory, arguments.deleteUserFiles);
            return 0;
        }

        options.portable = arguments.portable;
        options.reshade = options.presets = false;
        bool depth = false, dlss5 = false;
        const std::wstring components = arguments.components.empty() ? L"reshade,presets" : arguments.components;
        for (size_t start = 0; start <= components.size();)
        {
            size_t end = components.find(L',', start);
            end = end == std::wstring::npos ? components.size() : end;
            std::wstring component = components.substr(start, end - start);
            start = end + 1;
            CharLowerBuffW(component.data(), static_cast<DWORD>(component.size()));
            if (component == L"reshade")
                options.reshade = true;
            else if (component == L"presets")
                options.presets = true;
            else if (component == L"depth")
                depth = true;
            else if (component == L"dlss5")
                dlss5 = true;
            else if (component != L"host" && !component.empty())
                throw std::runtime_error("Unknown component: " + Utf8(component) + ". Use reshade, presets, depth or dlss5.");
        }
        if (depth && dlss5)
            throw std::runtime_error("Depth estimation and DLSS5 do not work together. Pick one.");
        options.addon = depth ? Addon::Depth : dlss5 ? Addon::DLSS5 : Addon::None;
        if (!options.reshade && (options.presets || options.addon != Addon::None))
            throw std::runtime_error("Presets and add-ons need ReShade. Add reshade to --components.");
        if (options.reshade && !arguments.acceptLicense)
            throw std::runtime_error("Installing ReShade needs --accept-reshade-license. Read the license first: "
                                     "https://github.com/crosire/reshade/blob/main/LICENSE.md");

        Progress progress;
        const ReShadeRelease release = options.reshade ? FetchReShadeRelease(progress.cancel) : ReShadeRelease{};
        Install(options, release, progress);
        return 0;
    }
    catch (const Cancelled&)
    {
        SetupLog("Cancelled.");
    }
    catch (const std::exception& e)
    {
        SetupLog(std::string("Error: ") + e.what());
    }
    return 1;
}

Arguments ParseArguments()
{
    Arguments arguments;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; i < count; ++i)
    {
        const std::wstring argument = argv[i];
        const auto value = [&](std::wstring& target) {
            if (i + 1 < count)
                target = argv[++i];
            else
                arguments.error = Utf8(argument) + " needs a value.";
        };
        if (argument == L"--silent")
            arguments.silent = true;
        else if (argument == L"--uninstall")
            arguments.uninstall = true;
        else if (argument == L"--delete-user-files")
            arguments.deleteUserFiles = true;
        else if (argument == L"--accept-reshade-license")
            arguments.acceptLicense = true;
        else if (argument == L"--portable")
            arguments.portable = true;
        else if (argument == L"--dir")
            value(arguments.directory);
        else if (argument == L"--components")
            value(arguments.components);
        else if (argument == L"--log")
            value(arguments.log);
        else if (argument == L"--effects-url")
            value(sources.effects);
        else if (argument == L"--presets-url")
            value(sources.presets);
        else if (argument == L"--dlss5-manifest")
            value(sources.dlss5);
        else if (argument == L"--depth-manifest")
            value(sources.depth);
        else
            arguments.error = "Unknown option: " + Utf8(argument);
    }
    LocalFree(argv);
    return arguments;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const Arguments arguments = ParseArguments();
    OpenSetupLog(!arguments.log.empty() ? fs::absolute(arguments.log) : fs::temp_directory_path() / L"RobloxShadeHost-Setup.log");
    SetupLog("RobloxShadeHost Setup " ROBLOX_SHADE_HOST_VERSION);
    if (!arguments.error.empty())
    {
        SetupLog(arguments.error);
        if (!arguments.silent)
            MessageBoxW(nullptr, Wide(arguments.error).c_str(), L"RobloxShadeHost Setup", MB_ICONERROR);
        return 1;
    }
    const int result = arguments.silent ? RunSilent(arguments) : RunWindow(arguments);
    DeleteMovedSetup();
    return result;
}
