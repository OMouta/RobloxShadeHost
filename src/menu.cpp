// The host's menu. ReShade runs the reshade_overlay event every frame after the effects, so the menu is drawn
// with ReShade's own ImGui on top of the processed picture. ReShade's menu stays one click away for
// everything this one leaves out.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "menu.h"
#include "addon.h"
#include "config.h"
#include "log.h"
#include "reshade_imgui.h"
#include "resource.h"
#include "shell.h"
#include "state.h"
#include "theme.h"
#include "update.h"
#include "../installer/text.h"

#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace reshade::api;

namespace
{
constexpr ImU32 Color(unsigned rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

constexpr ImU32 kBackground = Color(theme::kBackground);
constexpr ImU32 kInset = Color(theme::kSidebar);
constexpr ImU32 kCard = Color(theme::kCard);
constexpr ImU32 kCardHover = Color(theme::kCardHover);
constexpr ImU32 kBorder = Color(theme::kBorder);
constexpr ImU32 kBorderStrong = Color(theme::kBorderStrong);
constexpr ImU32 kText = Color(theme::kText);
constexpr ImU32 kDim = Color(theme::kDim);
constexpr ImU32 kAccent = Color(theme::kAccent);
constexpr ImU32 kAccentHover = Color(theme::kAccentHover);
constexpr ImU32 kAccentActive = Color(theme::kAccentActive);
constexpr ImU32 kWarning = Color(theme::kWarning);
constexpr ImU32 kError = Color(theme::kError);
constexpr ImU32 kSuccess = Color(theme::kSuccess);

// Layout in pixels at 100% scaling.
constexpr float kWidth = 460;
constexpr float kMargin = 16;
constexpr float kPadding = 18;
constexpr float kHeader = 74;
constexpr float kTabs = 42;
constexpr float kFooter = 64;
constexpr ULONGLONG kHintDuration = 6000;

// The DLSS5 add-on as ReShade loads it, and the title of its window in ReShade's menu.
constexpr wchar_t kDlssModule[] = L"renodx-dlss.addon64";
constexpr char kDlssWindow[] = "RenoDX DLSS";

enum class Tab
{
    Presets,
    Effects,
    Settings,
    Status,
};

struct Technique
{
    effect_technique handle{};
    std::string key; // name@file, like ReShade's presets, stays the same across reloads
    std::string label;
    std::string effect;
    std::string tooltip;
    std::string search; // lowercase label and file name
    bool failed = false;
};

struct Parameter
{
    effect_uniform_variable handle{};
    format base = format::unknown;
    uint32_t rows = 0;
    uint32_t columns = 0;
    uint32_t arrayLength = 0;
    std::string label;
    std::string tooltip;
    std::string category;
    std::string type;
    std::string text;
    std::string units;
    std::string items; // separated by '\0', as ImGui::Combo takes them
    float min = 0;
    float max = 0;
    float step = 0;
    int spacing = 0;
    bool categoryClosed = false;
    bool noReset = false;
};

enum class NameAction
{
    New,
    Duplicate,
    Rename,
    SaveAsNew,
};

// What to do with unsaved changes before switching presets.
enum class UnsavedChoice
{
    Ask,
    Save,
    Discard,
};

struct Menu
{
    effect_runtime* runtime = nullptr;
    float scale = 1;
    ImGuiMouseCursor cursor = ImGuiMouseCursor_Arrow;
    ULONGLONG hintStart = 0;
    bool hintShown = false;
    Tab tab = Tab::Presets;

    // Handles become invalid when ReShade reloads effects, so everything is read again after a reload.
    bool techniquesDirty = true;
    std::vector<Technique> techniques;
    std::vector<size_t> byName;
    std::string expanded;
    std::string parametersEffect;
    std::vector<Parameter> parameters;
    char search[128]{};
    bool showAll = false;
    // Keys of the effects listed under Active since the menu opened or the preset changed.
    std::set<std::string> active;
    bool presetChanged = false;
    // With auto-save off, changes wait for the save icon. ReShade saves the current preset whenever it switches
    // to another one, so a switch first asks what to do with them.
    bool autoSave = true;
    bool unsaved = false;
    UnsavedChoice unsavedChoice = UnsavedChoice::Ask;
    bool openUnsavedPopup = false;
    bool askingUnsaved = false;
    // Set when the changes went into the preset being switched to, so the one left behind is reverted.
    bool pendingKeepsEdits = false;

    std::vector<fs::path> presets;
    ULONGLONG presetsScanned = 0;
    // Switching presets can reload effects, so it waits until the frame is drawn.
    fs::path pendingPreset;
    bool saveNewPreset = false;
    bool openNamePopup = false;
    NameAction nameAction = NameAction::New;
    fs::path nameTarget;
    char name[128]{};
    std::string nameError;
    bool openDeletePopup = false;
    fs::path deleteTarget;
    std::string deleteError;

    bool comparing = false;
    bool effectsBeforeCompare = true;
    bool openReShade = false;
    bool openDlss = false;
    bool focusDlss = false;

    int capturing = -1;
    std::wstring shortcutError;

    resource logo{};
    resource_view logoView{};
    int logoSize = 0;
};
Menu m;

float S(float value)
{
    return value * m.scale;
}

std::string Lower(std::string text)
{
    for (char& character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

// Logo

std::vector<BYTE> LogoPixels(UINT size)
{
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    const HGLOBAL resource = info ? LoadResource(nullptr, info) : nullptr;
    if (!resource)
        return {};
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> premultiplied;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> straight;
    std::vector<BYTE> pixels(size * size * 4);
    // Scaling with premultiplied alpha keeps the transparent edge from darkening.
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(resource)), SizeofResource(nullptr, info))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(factory->CreateFormatConverter(premultiplied.put())) ||
        FAILED(premultiplied->Initialize(frame.get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(premultiplied.get(), size, size, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(straight.put())) ||
        FAILED(straight->Initialize(scaler.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(straight->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return {};
    return pixels;
}

void DestroyLogo()
{
    if (m.runtime && m.logoView.handle)
        m.runtime->get_device()->destroy_resource_view(m.logoView);
    if (m.runtime && m.logo.handle)
        m.runtime->get_device()->destroy_resource(m.logo);
    m.logo = {};
    m.logoView = {};
    m.logoSize = 0;
}

// Made at the size it is drawn, since ImGui draws textures without mipmaps.
void UpdateLogo(int size)
{
    if (size == m.logoSize)
        return;
    DestroyLogo();
    m.logoSize = size;
    std::vector<BYTE> pixels = LogoPixels(size);
    if (pixels.empty())
        return;
    device* device = m.runtime->get_device();
    const subresource_data data{ pixels.data(), static_cast<uint32_t>(size * 4), 0 };
    const resource_desc desc(size, size, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::default_, resource_usage::shader_resource);
    if (device->create_resource(desc, &data, resource_usage::shader_resource, &m.logo))
        device->create_resource_view(m.logo, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm), &m.logoView);
}

// Drawing helpers

void ApplyStyle(ImGuiStyle& style)
{
    style.Alpha = 1;
    style.DisabledAlpha = 0.45f;
    style.WindowPadding = ImVec2(0, 0);
    style.WindowRounding = S(12);
    style.WindowBorderSize = 1;
    style.WindowMinSize = ImVec2(1, 1);
    style.ChildRounding = S(8);
    style.ChildBorderSize = 1;
    style.PopupRounding = S(10);
    style.PopupBorderSize = 1;
    style.FramePadding = ImVec2(S(10), S(6));
    style.FrameRounding = S(6);
    style.FrameBorderSize = 0;
    style.ItemSpacing = ImVec2(S(8), S(8));
    style.ItemInnerSpacing = ImVec2(S(8), S(6));
    style.IndentSpacing = S(16);
    style.ScrollbarSize = S(8);
    style.ScrollbarRounding = S(4);
    style.GrabMinSize = S(10);
    style.GrabRounding = S(4);
    style.TabRounding = S(6);
    // ReShade scales its own text by resolution. The menu sizes its text itself.
    style.FontScaleMain = 1;
    style.FontScaleDpi = 1;

    const auto color = [&](ImGuiCol index, ImU32 value) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(value); };
    for (ImGuiCol index = 0; index < ImGuiCol_COUNT; ++index)
        color(index, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_Text, kText);
    color(ImGuiCol_TextDisabled, kDim);
    color(ImGuiCol_WindowBg, kBackground);
    color(ImGuiCol_PopupBg, Color(0x16171D, 252));
    color(ImGuiCol_Border, kBorder);
    color(ImGuiCol_FrameBg, kCard);
    color(ImGuiCol_FrameBgHovered, kCardHover);
    color(ImGuiCol_FrameBgActive, kCardHover);
    color(ImGuiCol_ScrollbarGrab, kBorder);
    color(ImGuiCol_ScrollbarGrabHovered, kBorderStrong);
    color(ImGuiCol_ScrollbarGrabActive, kBorderStrong);
    color(ImGuiCol_CheckMark, IM_COL32_WHITE);
    color(ImGuiCol_SliderGrab, kAccent);
    color(ImGuiCol_SliderGrabActive, kAccentHover);
    color(ImGuiCol_Button, kCard);
    color(ImGuiCol_ButtonHovered, kCardHover);
    color(ImGuiCol_ButtonActive, kBorder);
    color(ImGuiCol_Header, kCard);
    color(ImGuiCol_HeaderHovered, kCardHover);
    color(ImGuiCol_HeaderActive, kBorder);
    color(ImGuiCol_Separator, kBorder);
    color(ImGuiCol_TextLink, kAccentHover);
    color(ImGuiCol_TextSelectedBg, Color(theme::kAccent, 90));
    color(ImGuiCol_InputTextCursor, kText);
    color(ImGuiCol_NavCursor, kAccent);
    color(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 120));
}

void PushSize(float size)
{
    ImGui::PushFont(nullptr, S(size));
}

// Wraps at the edge of the window, at wrap in window coordinates when given, or not at all when wrap is negative.
void Text(const std::string& text, ImU32 color = kText, float size = 14.5f, float wrap = 0.0f)
{
    PushSize(size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Heading(const char* text)
{
    ImGui::Dummy(ImVec2(0, S(2)));
    Text(text, kDim, 12);
}

void HandOnHover()
{
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

bool Button(const char* label, ImVec2 size, bool primary = false, bool enabled = true)
{
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? kAccent : kCard);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? kAccentHover : kCardHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? kAccentActive : kBorder);
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? IM_COL32_WHITE : kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, primary ? 0.0f : 1.0f);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    if (enabled)
        HandOnHover();
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

bool Link(const char* label, ImU32 color = kAccentHover)
{
    ImGui::PushStyleColor(ImGuiCol_TextLink, color);
    const bool clicked = ImGui::TextLink(label);
    HandOnHover();
    ImGui::PopStyleColor();
    return clicked;
}

// An on/off switch. Returns true when clicked.
bool Switch(const char* id, bool on)
{
    const ImVec2 size(S(32), S(18));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, position + size, on ? (hovered ? kAccentHover : kAccent) : (hovered ? kBorderStrong : kBorder), size.y / 2);
    const float x = on ? position.x + size.x - size.y / 2 : position.x + size.y / 2;
    draw->AddCircleFilled(ImVec2(x, position.y + size.y / 2), size.y / 2 - S(3), on ? IM_COL32_WHITE : kDim);
    return clicked;
}

void KeyCap(const std::string& key, float size = 13)
{
    PushSize(size);
    const ImVec2 text = ImGui::CalcTextSize(key.c_str());
    const ImVec2 box(std::max(text.x + S(14), S(28)), text.y + S(6));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, position + box, kCard, S(5));
    draw->AddRect(position, position + box, kBorderStrong, S(5));
    draw->AddText(position + (box - text) * 0.5f, kText, key.c_str());
    ImGui::Dummy(box);
    ImGui::PopFont();
}

void Spinner(float radius)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(radius, radius);
    const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(center, radius - S(2), start, start + 4.2f, 32);
    draw->PathStroke(kAccent, 0, S(2.5f));
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

// A floppy disk: an outline with one corner cut, the shutter at the top and the label below.
void SaveIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 min = center - ImVec2(size, size) * 0.5f;
    const ImVec2 max = center + ImVec2(size, size) * 0.5f;
    const float corner = size * 0.25f;
    const ImVec2 outline[] = { min, ImVec2(max.x - corner, min.y), ImVec2(max.x, min.y + corner), max, ImVec2(min.x, max.y) };
    draw->AddPolyline(outline, 5, color, ImDrawFlags_Closed, S(1.5f));
    draw->AddRectFilled(ImVec2(min.x + size * 0.25f, min.y), ImVec2(max.x - size * 0.35f, min.y + size * 0.3f), color);
    draw->AddRect(ImVec2(min.x + size * 0.2f, center.y + size * 0.1f), ImVec2(max.x - size * 0.2f, max.y), color, 0, 0, S(1.5f));
}

void Rainbow(ImDrawList* draw, ImVec2 min, ImVec2 max)
{
    constexpr int count = static_cast<int>(std::size(theme::kRainbow));
    for (int i = 0; i + 1 < count; ++i)
    {
        const float left = min.x + (max.x - min.x) * i / (count - 1);
        const float right = min.x + (max.x - min.x) * (i + 1) / (count - 1);
        const ImU32 from = Color(theme::kRainbow[i]);
        const ImU32 to = Color(theme::kRainbow[i + 1]);
        draw->AddRectFilledMultiColor(ImVec2(left, min.y), ImVec2(right, max.y), from, to, to, from);
    }
}

// A check, exclamation mark or cross in a tinted circle, for the status list.
void NoticeIcon(ImDrawList* draw, ImVec2 center, LogLevel level)
{
    const float r = S(8);
    const ImU32 color = level == LogLevel::Ok ? kSuccess : level == LogLevel::Warning ? kWarning : level == LogLevel::Error ? kError : kDim;
    draw->AddCircleFilled(center, r, (color & 0x00FFFFFF) | 0x30000000);
    const float t = S(1.6f);
    switch (level)
    {
    case LogLevel::Ok:
    {
        const ImVec2 points[] = { center + ImVec2(-S(3.5f), 0), center + ImVec2(-S(1), S(2.8f)), center + ImVec2(S(3.8f), -S(2.8f)) };
        draw->AddPolyline(points, 3, color, 0, t);
        break;
    }
    case LogLevel::Warning:
        draw->AddLine(center + ImVec2(0, -S(4)), center + ImVec2(0, S(1)), color, t);
        draw->AddCircleFilled(center + ImVec2(0, S(3.8f)), S(1.1f), color);
        break;
    case LogLevel::Error:
        draw->AddLine(center + ImVec2(-S(3), -S(3)), center + ImVec2(S(3), S(3)), color, t);
        draw->AddLine(center + ImVec2(S(3), -S(3)), center + ImVec2(-S(3), S(3)), color, t);
        break;
    default:
        draw->AddCircleFilled(center, S(2.2f), color);
        break;
    }
}

bool HasProblems()
{
    const auto notices = Notices();
    return std::any_of(notices.begin(), notices.end(), [](const Notice& notice) { return notice.level >= LogLevel::Warning; });
}

// Presets

fs::path CurrentPreset()
{
    size_t size = 0;
    m.runtime->get_current_preset_path(nullptr, &size);
    std::string path(size, '\0');
    m.runtime->get_current_preset_path(path.data(), &size);
    path.resize(size);
    return fs::path(Wide(path));
}

void SavePreset()
{
    m.runtime->save_current_preset();
    m.presetChanged = false;
    m.unsaved = false;
}

// Loads the active preset again as it was last saved. ReShade only saves the preset it leaves when switching to
// a different one, so switching to the same one discards the changes.
void DiscardChanges()
{
    m.runtime->set_current_preset_path(Utf8(CurrentPreset().wstring()).c_str());
    m.presetChanged = false;
    m.unsaved = false;
    m.active.clear();
}

bool IsPreset(const fs::path& path)
{
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
        if (line.rfind("Techniques=", 0) == 0)
            return true;
    return false;
}

bool SamePath(const fs::path& a, const fs::path& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

void ScanPresets(const fs::path& current)
{
    m.presets.clear();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(current.parent_path(), error))
        if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".ini") == 0 && IsPreset(entry.path()))
            m.presets.push_back(entry.path());
    // ReShade writes a new preset a moment after switching to it.
    if (std::none_of(m.presets.begin(), m.presets.end(), [&](const fs::path& preset) { return SamePath(preset, current); }))
        m.presets.push_back(current);
    std::sort(m.presets.begin(), m.presets.end(),
              [](const fs::path& a, const fs::path& b) { return _wcsicmp(a.stem().c_str(), b.stem().c_str()) < 0; });
    m.presetsScanned = GetTickCount64();
}

void OpenNamePopup(NameAction action, const fs::path& target)
{
    m.nameAction = action;
    m.nameTarget = target;
    m.nameError.clear();
    const std::string stem = Utf8(target.stem().wstring());
    const std::string name = action == NameAction::New    ? "New preset"
                             : action == NameAction::Rename ? stem
                                                            : stem + " copy";
    strncpy_s(m.name, name.c_str(), _TRUNCATE);
    m.openNamePopup = true;
}

bool ApplyName(const fs::path& current)
{
    std::wstring name = Wide(m.name);
    name.erase(0, name.find_first_not_of(L' '));
    name.erase(name.find_last_not_of(L' ') + 1);
    if (name.empty())
    {
        m.nameError = "Enter a name.";
        return false;
    }
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
    {
        m.nameError = "A name cannot contain \\ / : * ? \" < > |";
        return false;
    }
    const fs::path path = current.parent_path() / (name + L".ini");
    std::error_code error;
    if (fs::exists(path, error) && !(m.nameAction == NameAction::Rename && SamePath(path, m.nameTarget)))
    {
        m.nameError = "A preset with that name already exists.";
        return false;
    }

    switch (m.nameAction)
    {
    case NameAction::New:
        // Switching to a preset that does not exist yet starts with every effect off.
        m.pendingPreset = path;
        m.saveNewPreset = true;
        break;
    case NameAction::Duplicate:
    case NameAction::SaveAsNew:
        // The active preset is copied as it is on screen, unsaved changes included.
        if (SamePath(m.nameTarget, current))
        {
            m.runtime->export_current_preset(Utf8(path.wstring()).c_str());
            m.pendingKeepsEdits = true;
        }
        else
            fs::copy_file(m.nameTarget, path, error);
        m.pendingPreset = path;
        break;
    case NameAction::Rename:
        fs::rename(m.nameTarget, path, error);
        break;
    }
    if (error)
    {
        m.nameError = "Windows could not save the preset.";
        m.pendingPreset.clear();
        return false;
    }
    m.presetsScanned = 0;
    return true;
}

void NamePopup(const fs::path& current)
{
    if (m.openNamePopup)
    {
        ImGui::OpenPopup("##name");
        m.openNamePopup = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(380), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    if (ImGui::BeginPopupModal("##name", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        const char* titles[] = { "New preset", "Duplicate preset", "Rename preset", "Save as new preset" };
        const char* actions[] = { "Create", "Duplicate", "Rename", "Save" };
        const int action = static_cast<int>(m.nameAction);
        Text(titles[action], kText, 16.5f);
        if (m.nameAction == NameAction::New)
            Text("Starts with every effect off.", kDim, 13.5f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputText("##value", m.name, sizeof(m.name), ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m.nameError.empty())
            Text(m.nameError, kError, 13.5f);
        ImGui::Dummy(ImVec2(0, S(2)));
        const float buttonWidth = S(100);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(20) - buttonWidth * 2 - S(8));
        const bool cancel = Button("Cancel", ImVec2(buttonWidth, S(32)));
        ImGui::SameLine(0, S(8));
        if ((Button(actions[action], ImVec2(buttonWidth, S(32)), true) || enter) && ApplyName(current))
            ImGui::CloseCurrentPopup();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void DeletePopup()
{
    if (m.openDeletePopup)
    {
        ImGui::OpenPopup("##delete");
        m.openDeletePopup = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(380), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    if (ImGui::BeginPopupModal("##delete", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        Text("Delete " + Utf8(m.deleteTarget.stem().wstring()) + "?", kText, 16.5f);
        Text("The preset goes to the Recycle Bin.", kDim, 13.5f);
        if (!m.deleteError.empty())
            Text(m.deleteError, kError, 13.5f);
        ImGui::Dummy(ImVec2(0, S(2)));
        const float buttonWidth = S(100);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(20) - buttonWidth * 2 - S(8));
        const bool cancel = Button("Cancel", ImVec2(buttonWidth, S(32)));
        ImGui::SameLine(0, S(8));
        if (Button("Delete", ImVec2(buttonWidth, S(32)), true))
        {
            if (Recycle(m.deleteTarget.wstring()))
            {
                m.presetsScanned = 0;
                ImGui::CloseCurrentPopup();
            }
            else
                m.deleteError = "Windows could not delete the preset.";
        }
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void PresetRow(const fs::path& path, bool active)
{
    const std::string name = Utf8(path.stem().wstring());
    ImGui::PushID(name.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(44));
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav) && !active)
        m.pendingPreset = path;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));
    draw->AddRect(start, start + size, active ? kAccent : kBorder, S(8), 0, active ? S(1.5f) : 1.0f);

    // The menu button, drawn as three dots.
    const ImVec2 dots = start + ImVec2(size.x - S(38), (size.y - S(28)) / 2);
    ImGui::SetCursorScreenPos(dots);
    if (ImGui::InvisibleButton("actions", ImVec2(S(28), S(28)), ImGuiButtonFlags_EnableNav))
        ImGui::OpenPopup("actions");
    const bool dotsHovered = ImGui::IsItemHovered();
    HandOnHover();
    if (dotsHovered)
        draw->AddRectFilled(dots, dots + ImVec2(S(28), S(28)), kBorder, S(6));
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(dots + ImVec2(S(14) + i * S(5), S(14)), S(1.6f), hovered || dotsHovered ? kText : kDim);

    PushSize(14.5f);
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(dots.x - S(70), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(14), textY), kText, name.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    if (active)
    {
        PushSize(11.5f);
        const char* tag = "ACTIVE";
        const ImVec2 tagSize = ImGui::CalcTextSize(tag) + ImVec2(S(14), S(6));
        const ImVec2 tagStart(dots.x - S(8) - tagSize.x, start.y + (size.y - tagSize.y) / 2);
        draw->AddRectFilled(tagStart, tagStart + tagSize, Color(theme::kAccent, 50), tagSize.y / 2);
        draw->AddText(tagStart + ImVec2(S(7), S(3)), kAccentHover, tag);
        ImGui::PopFont();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("actions"))
    {
        if (ImGui::MenuItem("Duplicate"))
            OpenNamePopup(NameAction::Duplicate, path);
        if (ImGui::MenuItem("Rename", nullptr, false, !active))
            OpenNamePopup(NameAction::Rename, path);
        if (ImGui::MenuItem("Delete", nullptr, false, !active))
        {
            m.deleteTarget = path;
            m.deleteError.clear();
            m.openDeletePopup = true;
        }
        if (active)
        {
            ImGui::Separator();
            PushSize(12.5f);
            ImGui::TextDisabled("Switch to another preset to\nrename or delete this one.");
            ImGui::PopFont();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    ImGui::PopID();
}

// Asks what to do with unsaved changes when switching presets. The switch waits for the answer.
void UnsavedPopup(const fs::path& current)
{
    if (m.openUnsavedPopup)
    {
        ImGui::OpenPopup("##unsaved");
        m.openUnsavedPopup = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(420), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    if (ImGui::BeginPopupModal("##unsaved", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        Text("Save your changes to " + Utf8(current.stem().wstring()) + "?", kText, 16.5f);
        Text("Discarding goes back to how the preset was last saved.", kDim, 13.5f);
        ImGui::Dummy(ImVec2(0, S(2)));
        const float buttonWidth = S(100);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(20) - buttonWidth * 3 - S(16));
        if (Button("Discard", ImVec2(buttonWidth, S(32))))
            m.unsavedChoice = UnsavedChoice::Discard;
        ImGui::SameLine(0, S(8));
        if (Button("Cancel", ImVec2(buttonWidth, S(32))) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m.pendingPreset.clear();
            m.saveNewPreset = false;
            m.pendingKeepsEdits = false;
        }
        ImGui::SameLine(0, S(8));
        if (Button("Save", ImVec2(buttonWidth, S(32)), true))
            m.unsavedChoice = UnsavedChoice::Save;
        // Answered, or the menu closed and dropped the switch.
        if (m.unsavedChoice != UnsavedChoice::Ask || m.pendingPreset.empty())
        {
            m.askingUnsaved = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void PresetsTab()
{
    const fs::path current = CurrentPreset();
    if (GetTickCount64() - m.presetsScanned > 2000)
        ScanPresets(current);

    if (Button("New preset", ImVec2(S(130), S(32)), true))
        OpenNamePopup(NameAction::New, {});
    ImGui::SameLine(0, S(8));
    if (!m.autoSave)
    {
        if (Button("Save as new", ImVec2(S(120), S(32))))
            OpenNamePopup(NameAction::SaveAsNew, current);
        ImGui::SameLine(0, S(8));
    }
    if (Button("Open folder", ImVec2(S(120), S(32))))
        ShellOpen(current.parent_path().wstring());
    ImGui::Dummy(ImVec2(0, S(2)));

    for (const fs::path& preset : m.presets)
        PresetRow(preset, SamePath(preset, current));

    ImGui::Dummy(ImVec2(0, S(4)));
    Text(m.autoSave ? "Changes save to the active preset as you make them."
                    : "Changes apply right away. Save them with the icon at the top.",
         kDim, 13);
    NamePopup(current);
    DeletePopup();
    UnsavedPopup(current);
}

// Effects

std::string TechniqueString(effect_runtime* runtime, effect_technique technique, const char* name)
{
    size_t size = 0;
    if (!runtime->get_annotation_string_from_technique(technique, name, nullptr, &size) || size <= 1)
        return {};
    std::string value(size, '\0');
    runtime->get_annotation_string_from_technique(technique, name, value.data(), &size);
    value.resize(size);
    return value;
}

std::string UniformString(effect_runtime* runtime, effect_uniform_variable variable, const char* name)
{
    size_t size = 0;
    if (!runtime->get_annotation_string_from_uniform_variable(variable, name, nullptr, &size) || size <= 1)
        return {};
    std::string value(size, '\0');
    runtime->get_annotation_string_from_uniform_variable(variable, name, value.data(), &size);
    value.resize(size);
    return value;
}

void LoadTechniques()
{
    m.techniques.clear();
    m.parameters.clear();
    m.parametersEffect.clear();
    // Returns nothing while ReShade compiles effects.
    m.runtime->enumerate_techniques(nullptr, [](effect_runtime* runtime, effect_technique handle) {
        int32_t hidden = 0;
        if (runtime->get_annotation_int_from_technique(handle, "hidden", &hidden, 1) && hidden)
            return;
        char name[256] = "";
        char effect[256] = "";
        runtime->get_technique_name(handle, name);
        runtime->get_technique_effect_name(handle, effect);
        Technique technique;
        technique.handle = handle;
        technique.key = std::string(name) + "@" + effect;
        technique.label = TechniqueString(runtime, handle, "ui_label");
        if (technique.label.empty())
            technique.label = name;
        technique.effect = effect;
        technique.tooltip = TechniqueString(runtime, handle, "ui_tooltip");
        technique.search = Lower(technique.label + " " + technique.effect);
        m.techniques.push_back(std::move(technique));
    });
    m.byName.resize(m.techniques.size());
    for (size_t i = 0; i < m.byName.size(); ++i)
        m.byName[i] = i;
    std::sort(m.byName.begin(), m.byName.end(),
              [](size_t a, size_t b) { return _stricmp(m.techniques[a].label.c_str(), m.techniques[b].label.c_str()) < 0; });
    m.techniquesDirty = m.techniques.empty();
}

void LoadParameters(const std::string& effect)
{
    m.parameters.clear();
    m.parametersEffect = effect;
    m.runtime->enumerate_uniform_variables(effect.c_str(), [](effect_runtime* runtime, effect_uniform_variable handle) {
        // Variables with a source, such as the frame time, are set by ReShade itself.
        size_t size = 0;
        int32_t flag = 0;
        if (runtime->get_annotation_string_from_uniform_variable(handle, "source", nullptr, &size) ||
            (runtime->get_annotation_int_from_uniform_variable(handle, "hidden", &flag, 1) && flag))
            return;
        Parameter parameter;
        parameter.handle = handle;
        runtime->get_uniform_variable_type(handle, &parameter.base, &parameter.rows, &parameter.columns, &parameter.arrayLength);
        char name[256] = "";
        runtime->get_uniform_variable_name(handle, name);
        parameter.label = UniformString(runtime, handle, "ui_label");
        if (parameter.label.empty())
            parameter.label = name;
        parameter.tooltip = UniformString(runtime, handle, "ui_tooltip");
        parameter.category = UniformString(runtime, handle, "ui_category");
        parameter.type = UniformString(runtime, handle, "ui_type");
        parameter.text = UniformString(runtime, handle, "ui_text");
        parameter.units = UniformString(runtime, handle, "ui_units");
        parameter.items = UniformString(runtime, handle, "ui_items");
        if (!parameter.items.empty() && parameter.items.back() != '\0')
            parameter.items.push_back('\0');
        runtime->get_annotation_float_from_uniform_variable(handle, "ui_min", &parameter.min, 1);
        runtime->get_annotation_float_from_uniform_variable(handle, "ui_max", &parameter.max, 1);
        runtime->get_annotation_float_from_uniform_variable(handle, "ui_step", &parameter.step, 1);
        runtime->get_annotation_int_from_uniform_variable(handle, "ui_spacing", &parameter.spacing, 1);
        parameter.categoryClosed = runtime->get_annotation_int_from_uniform_variable(handle, "ui_category_closed", &flag, 1) && flag;
        parameter.noReset = runtime->get_annotation_int_from_uniform_variable(handle, "noreset", &flag, 1) && flag;
        m.parameters.push_back(std::move(parameter));
    });
}

// Draws the control for one variable of an effect. Returns true when its value changed.
bool DrawParameter(const Parameter& p)
{
    const bool list = p.type == "combo" || p.type == "list" || p.type == "radio";
    const bool supported = p.arrayLength == 0 && p.columns <= 1 && p.rows >= 1 && p.rows <= 4 && !(list && p.items.empty());
    if (p.spacing > 0)
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * p.spacing));
    if (!p.text.empty())
        Text(p.text, kDim, 13.5f);
    // Arrays and matrices are left to ReShade's menu, like variables that only carry text.
    if (!supported)
        return false;

    const float x = ImGui::GetCursorPosX();
    const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x + labelWidth - S(10));
    ImGui::PushStyleColor(ImGuiCol_Text, p.tooltip.empty() ? kText : Color(0xD8D9E6));
    ImGui::TextUnformatted(p.label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (!p.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", p.tooltip.c_str());
    ImGui::SameLine(x + labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    ImGui::PushID(reinterpret_cast<void*>(p.handle.handle));
    std::string units = p.units;
    for (size_t at = units.find('%'); at != std::string::npos; at = units.find('%', at + 2))
        units.insert(at, 1, '%');
    const bool range = p.min < p.max;
    const int count = static_cast<int>(p.rows);
    bool changed = false;
    switch (p.base)
    {
    case format::r32_typeless:
    {
        bool value = false;
        m.runtime->get_uniform_value_bool(p.handle, &value, 1);
        if (ImGui::Checkbox("##value", &value))
        {
            m.runtime->set_uniform_value_bool(p.handle, &value, 1);
            changed = true;
        }
        break;
    }
    case format::r32_float:
    {
        float values[4]{};
        m.runtime->get_uniform_value_float(p.handle, values, count);
        const std::string format = "%.3f" + units;
        if (p.type == "color" && count == 3)
            changed = ImGui::ColorEdit3("##value", values);
        else if (p.type == "color" && count == 4)
            changed = ImGui::ColorEdit4("##value", values, ImGuiColorEditFlags_AlphaBar);
        else if (list && count == 1)
        {
            int index = static_cast<int>(values[0]);
            changed = ImGui::Combo("##value", &index, p.items.c_str());
            values[0] = static_cast<float>(index);
        }
        else if (p.type == "input")
            changed = ImGui::InputScalarN("##value", ImGuiDataType_Float, values, count, nullptr, nullptr, format.c_str());
        else if (range && p.type != "drag")
            changed = ImGui::SliderScalarN("##value", ImGuiDataType_Float, values, count, &p.min, &p.max, format.c_str());
        else
        {
            const float speed = p.step > 0 ? p.step : range ? (p.max - p.min) / 300 : 0.01f;
            changed = ImGui::DragScalarN("##value", ImGuiDataType_Float, values, count, speed, range ? &p.min : nullptr,
                                         range ? &p.max : nullptr, format.c_str());
        }
        if (changed)
            m.runtime->set_uniform_value_float(p.handle, values, count);
        break;
    }
    case format::r32_sint:
    case format::r32_uint:
    {
        const bool isSigned = p.base == format::r32_sint;
        int32_t values[4]{};
        if (isSigned)
            m.runtime->get_uniform_value_int(p.handle, values, count);
        else
            m.runtime->get_uniform_value_uint(p.handle, reinterpret_cast<uint32_t*>(values), count);
        const ImGuiDataType type = isSigned ? ImGuiDataType_S32 : ImGuiDataType_U32;
        const std::string format = (isSigned ? "%d" : "%u") + units;
        const int32_t min = static_cast<int32_t>(p.min);
        const int32_t max = static_cast<int32_t>(p.max);
        if (list && count == 1)
            changed = ImGui::Combo("##value", values, p.items.c_str());
        else if (p.type == "input")
            changed = ImGui::InputScalarN("##value", type, values, count, nullptr, nullptr, format.c_str());
        else if (range && p.type != "drag")
            changed = ImGui::SliderScalarN("##value", type, values, count, &min, &max, format.c_str());
        else
            changed = ImGui::DragScalarN("##value", type, values, count, std::max(p.step, 1.0f), range ? &min : nullptr,
                                         range ? &max : nullptr, format.c_str());
        if (changed && isSigned)
            m.runtime->set_uniform_value_int(p.handle, values, count);
        else if (changed)
            m.runtime->set_uniform_value_uint(p.handle, reinterpret_cast<uint32_t*>(values), count);
        break;
    }
    default:
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
        break;
    }
    if (!p.noReset && ImGui::BeginPopupContextItem("reset"))
    {
        if (ImGui::MenuItem("Reset to default"))
        {
            m.runtime->reset_uniform_value(p.handle);
            changed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void DrawParameters(const Technique& technique)
{
    if (m.parametersEffect != technique.effect)
        LoadParameters(technique.effect);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    PushSize(13.5f);
    if (technique.failed)
        Text(technique.effect + " did not compile, so it cannot be turned on. ReShade's menu shows why.", kWarning, 13.5f);
    if (!technique.tooltip.empty())
        Text(technique.tooltip, kDim, 13.5f);
    if (m.parameters.empty())
        Text("This effect has no settings.", kDim, 13.5f);

    std::string category;
    bool open = true;
    for (const Parameter& parameter : m.parameters)
    {
        if (parameter.category != category)
        {
            category = parameter.category;
            open = category.empty() ||
                   ImGui::CollapsingHeader(category.c_str(), parameter.categoryClosed ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen);
        }
        if (open && DrawParameter(parameter))
            m.presetChanged = true;
    }
    if (!m.parameters.empty())
    {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (Link("Reset all", kDim))
        {
            for (const Parameter& parameter : m.parameters)
                if (!parameter.noReset)
                    m.runtime->reset_uniform_value(parameter.handle);
            m.presetChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Right-click a setting to reset only that one.");
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void TechniqueRow(Technique& technique)
{
    const bool on = m.runtime->get_technique_state(technique.handle);
    const bool expanded = m.expanded == technique.key;
    ImGui::PushID(technique.key.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(38));
    if (!expanded && !ImGui::IsRectVisible(size))
    {
        ImGui::Dummy(size);
        ImGui::PopID();
        return;
    }

    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("row", size, ImGuiButtonFlags_EnableNav))
        m.expanded = expanded ? std::string() : technique.key;
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || expanded)
        draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));

    ImGui::SetCursorScreenPos(start + ImVec2(S(10), (size.y - S(18)) / 2));
    if (Switch("on", on))
    {
        m.runtime->set_technique_state(technique.handle, !on);
        technique.failed = !on && !m.runtime->get_technique_state(technique.handle);
        m.presetChanged = true;
    }

    const float chevron = start.x + size.x - S(20);
    PushSize(14.5f);
    const ImVec2 labelSize = ImGui::CalcTextSize(technique.label.c_str());
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(chevron - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(52), textY), technique.failed ? kDim : kText, technique.label.c_str());
    ImGui::PopFont();
    PushSize(12.5f);
    draw->AddText(ImVec2(start.x + S(52) + labelSize.x + S(8), textY + S(1.5f)), technique.failed ? kWarning : kDim,
                  technique.failed ? "did not compile" : technique.effect.c_str());
    ImGui::PopFont();
    draw->PopClipRect();

    const ImVec2 c(chevron, start.y + size.y / 2);
    const float r = S(3.5f);
    if (expanded)
    {
        const ImVec2 points[] = { c + ImVec2(-r, -r / 2), c + ImVec2(0, r / 2), c + ImVec2(r, -r / 2) };
        draw->AddPolyline(points, 3, kText, 0, S(1.6f));
    }
    else
    {
        const ImVec2 points[] = { c + ImVec2(-r / 2, -r), c + ImVec2(r / 2, 0), c + ImVec2(-r / 2, r) };
        draw->AddPolyline(points, 3, hovered ? kText : kDim, 0, S(1.6f));
    }

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    if (expanded)
        DrawParameters(technique);
    ImGui::PopID();
}

void EffectsTab()
{
    if (m.techniquesDirty)
        LoadTechniques();

    PushSize(14.5f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search effects", m.search, sizeof(m.search));
    ImGui::PopFont();

    if (m.techniques.empty())
    {
        ImGui::Dummy(ImVec2(0, S(6)));
        Spinner(S(10));
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
        Text("Loading effects. The first time can take a minute.", kDim, 14);
        return;
    }

    const std::string filter = Lower(m.search);
    const auto matches = [&](const Technique& technique) { return filter.empty() || technique.search.find(filter) != std::string::npos; };

    // An effect turned off stays in Active until the menu closes, so it can be turned back on where it was.
    for (const Technique& technique : m.techniques)
        if (m.runtime->get_technique_state(technique.handle))
            m.active.insert(technique.key);
    const auto active = [&](const Technique& technique) { return m.active.count(technique.key) != 0; };

    Heading("ACTIVE");
    bool any = false;
    for (Technique& technique : m.techniques)
        if (active(technique) && matches(technique))
        {
            TechniqueRow(technique);
            any = true;
        }
    if (!any)
        Text(filter.empty() ? "No effects are on. Pick a preset or turn effects on below." : "No active effects match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    size_t others = 0;
    for (const Technique& technique : m.techniques)
        others += !active(technique) && matches(technique);
    const bool searching = !filter.empty();
    const std::string title = "ALL EFFECTS (" + std::to_string(others) + ")";
    PushSize(12);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::SetNextItemOpen(searching || m.showAll);
    const bool open = ImGui::TreeNodeEx((title + "###all").c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::IsItemToggledOpen() && !searching)
        m.showAll = !m.showAll;
    HandOnHover();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (open)
        for (size_t index : m.byName)
        {
            Technique& technique = m.techniques[index];
            if (!active(technique) && matches(technique))
                TechniqueRow(technique);
        }
}

// Settings

void StopCapture()
{
    m.capturing = -1;
    SuspendHotkeys(false);
}

void ApplyHotkeys(const InputHotkeys& hotkeys)
{
    if (hotkeys.overlay.key == hotkeys.input.key && hotkeys.overlay.modifiers == hotkeys.input.modifiers)
        m.shortcutError = L"Pick two different shortcuts.";
    else
        m.shortcutError = ChangeHotkeys(hotkeys);
}

// While the menu waits for a shortcut, the next key pressed with any modifiers becomes it.
void CaptureShortcut()
{
    if (m.capturing < 0)
        return;
    const uint32_t key = m.runtime->last_key_pressed();
    // Mouse buttons and modifiers on their own.
    if (key < 8 || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN || (key >= VK_LSHIFT && key <= VK_RMENU))
        return;
    Hotkey hotkey;
    if (m.runtime->is_key_down(VK_CONTROL))
        hotkey.modifiers |= MOD_CONTROL;
    if (m.runtime->is_key_down(VK_MENU))
        hotkey.modifiers |= MOD_ALT;
    if (m.runtime->is_key_down(VK_SHIFT))
        hotkey.modifiers |= MOD_SHIFT;
    if (m.runtime->is_key_down(VK_LWIN) || m.runtime->is_key_down(VK_RWIN))
        hotkey.modifiers |= MOD_WIN;
    if (key == VK_ESCAPE && hotkey.modifiers == MOD_NOREPEAT)
    {
        StopCapture();
        return;
    }
    hotkey.key = key;
    if (FormatHotkey(hotkey).empty())
    {
        m.shortcutError = L"That key cannot be used. Use a letter, number, F key other than F12, Home, End, Insert, Delete, Page Up, "
                          L"Page Down, Pause or Scroll Lock, with or without Ctrl, Alt, Shift or Win.";
        return;
    }
    InputHotkeys hotkeys = g.hotkeys;
    (m.capturing == 0 ? hotkeys.input : hotkeys.overlay) = hotkey;
    StopCapture();
    ApplyHotkeys(hotkeys);
}

void ShortcutRow(int index, const char* title, const char* description, const Hotkey& hotkey, bool clearable)
{
    ImGui::PushID(index);
    const float x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = S(130);
    const float controls = buttonWidth + (clearable ? S(36) : 0);

    ImGui::BeginGroup();
    Text(title, kText, 14.5f, x + width - controls - S(14));
    Text(description, kDim, 13, x + width - controls - S(14));
    ImGui::EndGroup();
    const float bottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos(ImVec2(x + width - controls, top + S(2)));
    const bool capturing = m.capturing == index;
    const std::string label = capturing ? "Press keys..." : hotkey.key ? Utf8(FormatHotkey(hotkey)) : "Not set";
    ImGui::PushStyleColor(ImGuiCol_Border, capturing ? kAccent : kBorderStrong);
    if (Button((label + "##key").c_str(), ImVec2(buttonWidth, S(32))))
    {
        if (capturing)
            StopCapture();
        else
        {
            m.capturing = index;
            m.shortcutError.clear();
            SuspendHotkeys(true);
        }
    }
    ImGui::PopStyleColor();
    if (clearable)
    {
        ImGui::SameLine(0, S(4));
        if (Button("x", ImVec2(S(32), S(32)), false, hotkey.key != 0))
        {
            InputHotkeys hotkeys = g.hotkeys;
            hotkeys.overlay = {};
            StopCapture();
            ApplyHotkeys(hotkeys);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Leave unassigned");
    }
    ImGui::SetCursorPos(ImVec2(x, std::max(bottom, top + S(40)) + S(6)));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

void SettingsTab()
{
    Heading("SHORTCUTS");
    Text("Click a shortcut, then press the keys you want. They work right away.", kDim, 13);
    ImGui::Dummy(ImVec2(0, S(4)));
    ShortcutRow(0, "Open the menu", "Press it again, or Escape, to go back to Roblox.", g.hotkeys.input, false);
    ShortcutRow(1, "Overlay off and on", "Shows Roblox without effects and stops capturing it.", g.hotkeys.overlay, true);

    const auto bare = [](const Hotkey& hotkey) { return hotkey.key && !(hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)); };
    const UINT key = g.hotkeys.input.key;
    const bool typing = key == VK_SPACE || key == VK_TAB || (key >= '0' && key <= 'Z');
    if (!m.shortcutError.empty())
        Text(Utf8(m.shortcutError), kError, 13.5f);
    if (bare(g.hotkeys.input) && typing)
        Text("Roblox will not receive " + Utf8(g.inputHotkey) + " while RobloxShadeHost runs.", kWarning, 13.5f);
    if (bare(g.hotkeys.overlay))
        Text("Other programs will not receive " + Utf8(g.overlayHotkey) + " while RobloxShadeHost runs.", kWarning, 13.5f);
    if (Link("Reset to defaults", kDim))
    {
        InputHotkeys hotkeys;
        ParseHotkey(kDefaultToggleKey, hotkeys.input);
        ParseHotkey(kDefaultOverlayToggleKey, hotkeys.overlay);
        StopCapture();
        ApplyHotkeys(hotkeys);
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    Heading("PRESETS");
    if (Switch("autosave", m.autoSave))
    {
        m.autoSave = !m.autoSave;
        SetAutoSavePresets(m.autoSave);
        // From here on every change saves as it happens, so changes that were waiting are saved too.
        if (m.autoSave && m.unsaved)
            SavePreset();
    }
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    Text("Save changes automatically", kText, 14.5f);
    Text("Turn off to try changes first and save them with the icon at the top.", kDim, 13);
    ImGui::EndGroup();
}

// Status

void StatusTab()
{
    const Update update = AvailableUpdate();
    if (!update.version.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(theme::kAccent, 30));
        ImGui::PushStyleColor(ImGuiCol_Border, Color(theme::kAccent, 120));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
        ImGui::BeginChild("update", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        Text("RobloxShadeHost " + Utf8(update.version) + " is available", kText, 14.5f);
        Text("Download the new Setup and run it. Your presets and settings stay.", kDim, 13);
        if (Button("Download", ImVec2(S(110), S(30)), true))
            ShellOpen(update.url);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0, S(2)));
    }

    Heading("STATUS");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PushSize(13.5f);
    for (const Notice& notice : Notices())
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        NoticeIcon(draw, start + ImVec2(S(8), ImGui::GetFontSize() / 2 + S(1)), notice.level);
        ImGui::SetCursorScreenPos(start + ImVec2(S(26), 0));
        ImGui::PushTextWrapPos(0.0f);
        const std::string text = Utf8(notice.text);
        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, S(4)));
    PushSize(13.5f);
    if (Link("Open log"))
        ShellOpen(LogPath());
    ImGui::SameLine(0, S(16));
    if (Link("Get help on Discord"))
        ShellOpen(kHelpUrl);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(6)));
    Text("RobloxShadeHost " ROBLOX_SHADE_HOST_VERSION ". Effects run on ReShade by crosire.", kDim, 12.5f);
}

// Frame

void Header(ImVec2 origin, float width)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float logo = std::round(S(38));
    UpdateLogo(static_cast<int>(logo));
    const ImVec2 logoPosition = origin + ImVec2(std::round(S(kPadding)), std::round(S(18)));
    if (m.logoView.handle)
        draw->AddImage(ImTextureRef(m.logoView.handle), logoPosition, logoPosition + ImVec2(logo, logo));
    const float textX = S(kPadding) + logo + S(12);
    PushSize(16.5f);
    draw->AddText(origin + ImVec2(textX, S(17)), kText, "RobloxShadeHost");
    const float titleWidth = ImGui::CalcTextSize("RobloxShadeHost").x;
    ImGui::PopFont();
    // The right side holds the effects switch, its label and, with auto-save off, the save icon.
    PushSize(13);
    const ImVec2 labelSize = ImGui::CalcTextSize("Effects");
    const float switchX = width - S(kPadding) - S(32);
    const float labelX = switchX - S(8) - labelSize.x;
    const ImVec2 saveSize(S(30), S(30));
    const float saveX = labelX - S(14) - saveSize.x;

    const std::string preset = Utf8(CurrentPreset().stem().wstring());
    const ImVec2 nameSize = ImGui::CalcTextSize(preset.c_str());
    draw->PushClipRect(origin, origin + ImVec2((m.autoSave ? labelX : saveX) - S(12), S(kHeader)), true);
    draw->AddText(origin + ImVec2(textX, S(39)), kDim, preset.c_str());
    if (m.unsaved)
        draw->AddCircleFilled(origin + ImVec2(textX + nameSize.x + S(7), S(39) + ImGui::GetFontSize() / 2 + S(1)), S(3), kWarning);
    draw->PopClipRect();
    ImGui::PopFont();

    if (!m.autoSave)
    {
        const ImVec2 start = origin + ImVec2(saveX, S(22));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton("save", saveSize, ImGuiButtonFlags_EnableNav) && m.unsaved)
            SavePreset();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(m.unsaved ? "Save changes to %s" : "No unsaved changes in %s", preset.c_str());
            if (m.unsaved)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                draw->AddRectFilled(start, start + saveSize, kBorder, S(6));
            }
        }
        SaveIcon(draw, start + saveSize * 0.5f, S(14), m.unsaved ? kAccentHover : kBorderStrong);
    }

    const Update update = AvailableUpdate();
    if (!update.version.empty())
    {
        PushSize(11.5f);
        const char* tag = "UPDATE";
        const ImVec2 tagSize = ImGui::CalcTextSize(tag) + ImVec2(S(12), S(5));
        const ImVec2 tagStart = origin + ImVec2(textX + titleWidth + S(8), S(19));
        ImGui::SetCursorScreenPos(tagStart);
        if (ImGui::InvisibleButton("update", tagSize, ImGuiButtonFlags_EnableNav))
            m.tab = Tab::Status;
        HandOnHover();
        draw->AddRectFilled(tagStart, tagStart + tagSize, Color(theme::kAccent, ImGui::IsItemHovered() ? 90 : 50), tagSize.y / 2);
        draw->AddText(tagStart + ImVec2(S(6), S(2.5f)), kAccentHover, tag);
        ImGui::PopFont();
    }

    // Effects on and off for everything, like ReShade's own shortcut.
    const bool on = m.runtime->get_effects_state() || m.comparing;
    ImGui::SetCursorScreenPos(origin + ImVec2(switchX, S(28)));
    if (Switch("effects", on) && !m.comparing)
        m.runtime->set_effects_state(!on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(on ? "Turn all effects off" : "Turn effects back on");
    PushSize(13);
    draw->AddText(origin + ImVec2(labelX, S(28) + (S(18) - labelSize.y) / 2), kDim, "Effects");
    ImGui::PopFont();

    Rainbow(draw, origin + ImVec2(0, S(kHeader) - S(2)), origin + ImVec2(width, S(kHeader)));
}

void Tabs(ImVec2 origin, float width)
{
    struct Entry
    {
        const char* name;
        // Empty for DLSS5. RenoDX's add-on only draws its settings in ReShade's menu, so its tab opens that.
        std::optional<Tab> tab;
    };
    Entry entries[5];
    int count = 0;
    entries[count++] = { "Presets", Tab::Presets };
    entries[count++] = { "Effects", Tab::Effects };
    if (GetModuleHandleW(kDlssModule))
        entries[count++] = { "DLSS5", std::nullopt };
    entries[count++] = { "Settings", Tab::Settings };
    entries[count++] = { "Status", Tab::Status };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tabWidth = (width - S(kPadding) * 2) / count;
    const bool problems = HasProblems();
    PushSize(14);
    for (int i = 0; i < count; ++i)
    {
        const Entry& entry = entries[i];
        const ImVec2 start = origin + ImVec2(S(kPadding) + i * tabWidth, S(kHeader));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton(entry.name, ImVec2(tabWidth, S(kTabs)), ImGuiButtonFlags_EnableNav))
        {
            if (entry.tab)
                m.tab = *entry.tab;
            else
                m.openDlss = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        const bool active = entry.tab == m.tab;
        const ImVec2 text = ImGui::CalcTextSize(entry.name);
        const ImVec2 textStart = start + ImVec2((tabWidth - text.x) / 2, (S(kTabs) - text.y) / 2);
        draw->AddText(textStart, active || hovered ? kText : kDim, entry.name);
        if (active)
            draw->AddRectFilled(ImVec2(textStart.x - S(6), start.y + S(kTabs) - S(2)), ImVec2(textStart.x + text.x + S(6), start.y + S(kTabs)),
                                kAccent, S(1));
        if (entry.tab == Tab::Status && problems)
            draw->AddCircleFilled(textStart + ImVec2(text.x + S(6), S(3)), S(3), kWarning);
    }
    ImGui::PopFont();
    draw->AddLine(origin + ImVec2(0, S(kHeader + kTabs)), origin + ImVec2(width, S(kHeader + kTabs)), kBorder);
}

// Effects stay off while the compare button is held.
void Compare(bool holding)
{
    if (holding == m.comparing)
        return;
    if (holding)
    {
        m.effectsBeforeCompare = m.runtime->get_effects_state();
        m.runtime->set_effects_state(false);
    }
    else
        m.runtime->set_effects_state(m.effectsBeforeCompare);
    m.comparing = holding;
}

void Footer(ImVec2 origin, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float top = size.y - S(kFooter);
    draw->AddLine(origin + ImVec2(0, top), origin + ImVec2(size.x, top), kBorder);
    const float buttonY = top + (S(kFooter) - S(34)) / 2;

    ImGui::SetCursorScreenPos(origin + ImVec2(S(kPadding), buttonY));
    PushSize(14);
    Button("Compare", ImVec2(S(100), S(34)));
    Compare(ImGui::IsItemActive());
    if (ImGui::IsItemHovered() && !m.comparing)
        ImGui::SetTooltip("Hold to see Roblox without effects");
    ImGui::SameLine(0, S(8));
    if (Button("ReShade", ImVec2(S(90), S(34))))
        m.openReShade = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open ReShade's own menu for everything else");
    ImGui::PopFont();

    // The menu shortcut, which also leaves.
    PushSize(13.5f);
    const char* label = "Back to Roblox";
    const std::string key = Utf8(g.inputHotkey);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    PushSize(13);
    const float keyWidth = std::max(ImGui::CalcTextSize(key.c_str()).x + S(14), S(28));
    ImGui::PopFont();
    const ImVec2 backSize(keyWidth + S(8) + labelSize.x, S(34));
    const ImVec2 backStart = origin + ImVec2(size.x - S(kPadding) - backSize.x, buttonY);
    ImGui::SetCursorScreenPos(backStart);
    if (ImGui::InvisibleButton("back", backSize, ImGuiButtonFlags_EnableNav))
        PostMessageW(g.overlay, kLeaveMenuMessage, 0, 0);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImGui::SetCursorScreenPos(backStart + ImVec2(0, (backSize.y - S(22)) / 2));
    KeyCap(key);
    PushSize(13.5f);
    draw->AddText(backStart + ImVec2(keyWidth + S(8), (backSize.y - labelSize.y) / 2), hovered ? kText : kDim, label);
    ImGui::PopFont();
}

void DrawMenu()
{
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(S(kWidth), io.DisplaySize.x - S(kMargin) * 2);
    const ImVec2 size(width, io.DisplaySize.y - S(kMargin) * 2);
    ImGui::SetNextWindowPos(ImVec2(S(kMargin), S(kMargin)));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("RobloxShadeHost##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetWindowPos();
    Header(origin, width);
    Tabs(origin, width);

    const float top = S(kHeader + kTabs) + 1;
    ImGui::SetCursorScreenPos(origin + ImVec2(0, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(kPadding), S(16)));
    ImGui::BeginChild("content", ImVec2(width, size.y - top - S(kFooter)), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar();
    switch (m.tab)
    {
    case Tab::Presets: PresetsTab(); break;
    case Tab::Effects: EffectsTab(); break;
    case Tab::Settings: SettingsTab(); break;
    case Tab::Status: StatusTab(); break;
    }
    ImGui::EndChild();
    Footer(origin, size);
    ImGui::End();
}

void DrawHint(ULONGLONG elapsed)
{
    const float seconds = elapsed / 1000.0f;
    const float alpha = std::clamp(std::min(seconds / 0.25f, (kHintDuration / 1000.0f - seconds) / 0.6f), 0.0f, 1.0f);
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y - S(48)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    KeyCap(Utf8(g.inputHotkey), 14);
    ImGui::SameLine(0, S(10));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
    Text("opens the RobloxShadeHost menu", kText, 14.5f, -1.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DrawMenuFrame()
{
    // Escape leaves the menu, unless it closes a popup, ends typing or cancels waiting for a shortcut.
    if (m.capturing < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        PostMessageW(g.overlay, kLeaveMenuMessage, 0, 0);
    CaptureShortcut();

    DrawMenu();

    if (m.capturing >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        StopCapture();
    // Saving writes the whole preset, so it waits until a slider is let go. With auto-save off, changes only mark
    // the preset as unsaved.
    if (m.presetChanged && (!ImGui::IsAnyItemActive() || !m.pendingPreset.empty()))
    {
        if (m.autoSave)
            SavePreset();
        else
            m.unsaved = true;
        m.presetChanged = false;
    }
    if (!m.pendingPreset.empty())
    {
        if (m.unsaved && !m.pendingKeepsEdits && m.unsavedChoice == UnsavedChoice::Ask)
        {
            if (!m.askingUnsaved)
            {
                m.askingUnsaved = true;
                m.openUnsavedPopup = true;
            }
        }
        else
        {
            // Changes that went into the new preset, or were discarded, must not be saved into this one.
            if (m.unsaved && (m.pendingKeepsEdits || m.unsavedChoice == UnsavedChoice::Discard))
                DiscardChanges();
            else if (m.unsaved)
                SavePreset();
            m.runtime->set_current_preset_path(Utf8(m.pendingPreset.wstring()).c_str());
            if (m.saveNewPreset)
                m.runtime->save_current_preset();
            m.pendingPreset.clear();
            m.saveNewPreset = false;
            m.pendingKeepsEdits = false;
            m.unsavedChoice = UnsavedChoice::Ask;
            m.presetsScanned = 0;
            m.active.clear();
        }
    }
    if (m.openReShade || m.openDlss)
    {
        m.focusDlss = m.openDlss;
        m.openReShade = m.openDlss = false;
        OpenReShadeMenu(true);
    }
}

void OnOverlay(effect_runtime* runtime)
{
    if (runtime != m.runtime)
        return;
    // ReShade draws the add-on's window before this runs, from the frame after its menu opens.
    if (m.focusDlss && ReShadeMenuOpen())
    {
        ImGui::SetWindowFocus(kDlssWindow);
        m.focusDlss = false;
    }
    const bool menu = g.editMode && !ReShadeMenuOpen();
    const ULONGLONG elapsed = GetTickCount64() - m.hintStart;
    const bool hint = !g.editMode && m.hintStart && elapsed < kHintDuration;
    if (!menu && !hint)
        return;

    // Small windows get a smaller menu, so it still fits.
    m.scale = std::clamp(std::min(GetDpiForWindow(g.overlay) / 96.0f, ImGui::GetIO().DisplaySize.y / 640.0f), 0.7f, 3.0f);
    // The menu's look only applies to its own windows, so ReShade's is restored after.
    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiStyle saved = style;
    ApplyStyle(style);
    PushSize(14.5f);
    if (menu)
        DrawMenuFrame();
    else
        DrawHint(elapsed);
    ImGui::PopFont();
    m.cursor = menu ? ImGui::GetMouseCursor() : ImGuiMouseCursor_Arrow;
    style = saved;
}

void OnInitRuntime(effect_runtime* runtime)
{
    m.runtime = runtime;
    m.techniquesDirty = true;
}

void OnDestroyRuntime(effect_runtime* runtime)
{
    if (runtime != m.runtime)
        return;
    DestroyLogo();
    m.runtime = nullptr;
    m.techniques.clear();
    m.parameters.clear();
    m.parametersEffect.clear();
}

void OnReloadedEffects(effect_runtime*)
{
    m.techniquesDirty = true;
    m.parameters.clear();
    m.parametersEffect.clear();
}

// Keys pressed while the menu waits for a shortcut still reach ReShade. This keeps End, ReShade's effects
// key, from turning effects off when it is picked as a shortcut.
bool OnSetEffectsState(effect_runtime*, bool)
{
    return m.capturing >= 0;
}
} // namespace

void InitMenu()
{
    if (!AddonRegistered())
        return;
    m.autoSave = AutoSavePresets();
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
    reshade::register_event<reshade::addon_event::reshade_set_effects_state>(OnSetEffectsState);
    reshade::register_event<reshade::addon_event::reshade_overlay>(OnOverlay);
}

void ShowStartHint()
{
    // Capture also restarts whenever the overlay is turned back on, often while taking comparison shots.
    if (!AddonRegistered() || m.hintShown)
        return;
    m.hintShown = true;
    m.hintStart = GetTickCount64();
}

void ResetMenu()
{
    if (m.capturing >= 0)
        StopCapture();
    m.focusDlss = false;
    m.active.clear();
    if (!m.runtime)
        return;
    if (m.comparing)
        m.runtime->set_effects_state(m.effectsBeforeCompare);
    m.comparing = false;
    if (m.presetChanged && m.autoSave)
        SavePreset();
    else if (m.presetChanged)
        m.unsaved = true;
    m.presetChanged = false;
    // A switch still waiting for an answer about unsaved changes is dropped.
    m.pendingPreset.clear();
    m.saveNewPreset = false;
    m.pendingKeepsEdits = false;
    m.unsavedChoice = UnsavedChoice::Ask;
}

LPCWSTR MenuCursor()
{
    if (ReShadeMenuOpen())
        return IDC_ARROW;
    switch (m.cursor)
    {
    case ImGuiMouseCursor_TextInput: return IDC_IBEAM;
    case ImGuiMouseCursor_Hand: return IDC_HAND;
    case ImGuiMouseCursor_ResizeAll: return IDC_SIZEALL;
    case ImGuiMouseCursor_ResizeEW: return IDC_SIZEWE;
    case ImGuiMouseCursor_ResizeNS: return IDC_SIZENS;
    case ImGuiMouseCursor_NotAllowed: return IDC_NO;
    default: return IDC_ARROW;
    }
}
