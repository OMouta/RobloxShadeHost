// The launcher and the menu, drawn the way the Windows host draws them: the launcher like src/launcher.cpp and the
// menu like src/menu.cpp, with the same layout, sizes and colors.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"
#include "log.h"
#include "menu_layout.h"
#include "theme.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <utility>

// assets/Unishade.png, which the build puts into the program.
extern const unsigned char kLogoPng[];
extern const unsigned kLogoPngSize;

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

// The menu's layout in pixels at 1080p.
using menu_layout::kWidth;
using menu_layout::kMargin;
using menu_layout::kHeader;
using menu_layout::kTabs;
using menu_layout::kFooter;
constexpr float kPadding = 18;

// The launcher's size at 100% scaling. Longer content makes the window taller, up to the screen, and then scrolls.
constexpr float kLauncherWidth = 620;
constexpr float kLauncherMinHeight = 540;
// The picker scrolls past its height.
constexpr float kPickerWidth = 480;
constexpr float kPickerMaxHeight = 520;
// How long a removed game can be put back.
constexpr double kUndoSeconds = 8;
// The launcher's text sizes. Windows gives them as the height of the letters' box, three quarters of the line height
// Dear ImGui sizes text by.
constexpr float kTitle = 19 * 1.33f;
constexpr float kSemibold = 15 * 1.33f;
constexpr float kBody = 13.5f * 1.33f;
constexpr float kNote = 12 * 1.33f;

// Help is given on the Unishade Discord server.
constexpr char kHelpUrl[] = "https://discord.gg/wVbVUdENas";

// A picture drawn in a window, made at the size it is drawn. Each takes a set from Dear ImGui's descriptor pool,
// which also holds its fonts, so a window has at most kMaxPictures.
constexpr size_t kMaxPictures = 192;
constexpr uint32_t kDescriptorPoolSize = kMaxPictures + 64;
constexpr char kLogoPicture[] = "logo";

struct Picture
{
    GpuImage image;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool used = false; // drawn since ForgetPictures last looked
};

// What each window's Dear ImGui context has of its own.
struct Context
{
    ImFont* bold = nullptr;
    // How much larger than asked text is drawn. See FontScale.
    float fontScale = 0;
    // The logo, and folders' logos, by folder and size in pixels: the launcher draws a game's icon at two sizes. A
    // folder without a logo keeps an empty entry until it is looked for again.
    std::map<std::pair<std::string, int>, Picture> pictures;
};
std::map<ImGuiContext*, Context> contexts;

Context& Current()
{
    return contexts[ImGui::GetCurrentContext()];
}

// The scale of the window being drawn.
float scale = 1;

float S(float value)
{
    return value * scale;
}

// Text sizes are the Windows host's, whose fonts have capitals 0.53 of the size tall. Fonts differ in that, DejaVu
// Sans and Noto Sans by a fifth, so the system's font is drawn at the size that makes its capitals as tall.
float FontScale()
{
    Context& context = Current();
    if (context.fontScale == 0)
    {
        context.fontScale = 1;
        const ImFontGlyph* capital = ImGui::GetFont()->GetFontBaked(100.0f)->FindGlyphNoFallback('H');
        if (capital && capital->Y1 > capital->Y0)
            context.fontScale = 53.0f / (capital->Y1 - capital->Y0);
    }
    return context.fontScale;
}

void PushSize(float size, bool bold = false)
{
    ImGui::PushFont(bold ? Current().bold : nullptr, S(size) * FontScale());
}

ImVec2 Measure(const std::string& text, float size, bool bold = false, float wrap = -1.0f)
{
    PushSize(size, bold);
    const ImVec2 result = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(), false, wrap);
    ImGui::PopFont();
    return result;
}

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
    // Text is sized by PushSize alone.
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
    color(ImGuiCol_DragDropTarget, kAccent);
    color(ImGuiCol_NavCursor, kAccent);
    color(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 120));
}

// Pictures

void DestroyPicture(Picture& picture)
{
    if (picture.set)
        ImGui_ImplVulkan_RemoveTexture(picture.set);
    gpu.DestroyImage(picture.image);
    picture = {};
}

// Averages the pixels each pixel of the result covers, weighted by alpha so transparent edges stay clean.
std::vector<uint8_t> Shrink(const uint8_t* rgba, int width, int height, int size)
{
    std::vector<uint8_t> result(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const int x0 = x * width / size, x1 = std::max(x0 + 1, (x + 1) * width / size);
            const int y0 = y * height / size, y1 = std::max(y0 + 1, (y + 1) * height / size);
            uint64_t color[3] = {}, alpha = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                {
                    const uint8_t* pixel = rgba + (size_t(sy) * width + sx) * 4;
                    for (int c = 0; c < 3; ++c)
                        color[c] += uint64_t(pixel[c]) * pixel[3];
                    alpha += pixel[3];
                }
            uint8_t* out = &result[(size_t(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c)
                out[c] = alpha ? uint8_t(color[c] / alpha) : 0;
            out[3] = uint8_t(alpha / (uint64_t(x1 - x0) * uint64_t(y1 - y0)));
        }
    return result;
}

// The logo, or a folder's logo.png, at a size in the window's own units. Invalid when the folder has none.
ImTextureID PictureTexture(const std::string& key, float drawn)
{
    const int size = static_cast<int>(std::round(drawn * ImGui::GetIO().DisplayFramebufferScale.x));
    auto& pictures = Current().pictures;
    const auto [entry, added] = pictures.try_emplace({ key, size });
    Picture& picture = entry->second;
    picture.used = true;
    if (!added || size <= 0)
        return (ImTextureID)picture.set;
    if (std::count_if(pictures.begin(), pictures.end(), [](const auto& other) { return other.second.set != VK_NULL_HANDLE; }) >=
        std::ptrdiff_t(kMaxPictures))
        return ImTextureID_Invalid;
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = key == kLogoPicture ? stbi_load_from_memory(kLogoPng, static_cast<int>(kLogoPngSize), &width, &height, &channels, 4)
                                          : stbi_load((fs::path(key) / "logo.png").c_str(), &width, &height, &channels, 4);
    if (!pixels)
        return ImTextureID_Invalid;
    const std::vector<uint8_t> rgba = Shrink(pixels, width, height, size);
    stbi_image_free(pixels);
    GpuBuffer upload;
    if (gpu.CreateImage(picture.image, size, size, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        gpu.CreateBuffer(upload, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
    {
        std::memcpy(upload.mapped, rgba.data(), rgba.size());
        if (VkCommandBuffer commands = gpu.BeginCommands())
        {
            InitLayout(commands, picture.image);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { uint32_t(size), uint32_t(size), 1 };
            vkCmdCopyBufferToImage(commands, upload.buffer, picture.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            FullBarrier(commands);
            if (gpu.SubmitAndWait(commands))
                picture.set = ImGui_ImplVulkan_AddTexture(picture.image.view, VK_IMAGE_LAYOUT_GENERAL);
            else
                vkDeviceWaitIdle(gpu.device); // commands that failed to finish may still use the image and buffer
        }
    }
    gpu.DestroyBuffer(upload);
    if (!picture.set)
        gpu.DestroyImage(picture.image);
    return (ImTextureID)picture.set;
}

// Drops the pictures not drawn since the last call, the folder logos that are not kept, and the folders found without
// one, so a logo saved since, such as the icon of a game that just started, shows the next time it is drawn.
void ForgetPictures(const std::function<bool(const std::string& folder)>& keep)
{
    auto& pictures = Current().pictures;
    bool waited = false;
    for (auto entry = pictures.begin(); entry != pictures.end();)
    {
        const std::string& folder = entry->first.first;
        if (std::exchange(entry->second.used, false) && entry->second.set && (folder == kLogoPicture || keep(folder)))
        {
            ++entry;
            continue;
        }
        if (entry->second.set && !std::exchange(waited, true))
            vkDeviceWaitIdle(gpu.device);
        DestroyPicture(entry->second);
        entry = pictures.erase(entry);
    }
}

// Drawing helpers

// Wraps at the edge of the window, at wrap in window coordinates when given, or not at all when wrap is negative.
void Text(const std::string& text, ImU32 color = kText, float size = 14.5f, float wrap = 0.0f, bool bold = false)
{
    PushSize(size, bold);
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

void Tooltip(const std::string& text)
{
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(7)));
    if (ImGui::BeginTooltip())
    {
        Text(text, kText, 13, S(300));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
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

void DrawSwitch(ImDrawList* draw, ImVec2 position, ImVec2 size, bool on, bool hovered)
{
    draw->AddRectFilled(position, position + size, on ? (hovered ? kAccentHover : kAccent) : (hovered ? kBorderStrong : kBorder), size.y / 2);
    const float x = on ? position.x + size.x - size.y / 2 : position.x + size.y / 2;
    draw->AddCircleFilled(ImVec2(x, position.y + size.y / 2), size.y / 2 - S(3), on ? IM_COL32_WHITE : kDim);
}

// An on/off switch. Returns true when clicked.
bool Switch(const char* id, bool on, ImVec2 size = ImVec2(0, 0))
{
    if (size.x <= 0)
        size = ImVec2(S(32), S(18));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    HandOnHover();
    DrawSwitch(ImGui::GetWindowDrawList(), position, size, on, ImGui::IsItemHovered());
    return clicked;
}

// Choices side by side in one control, each as wide as the others, with the selected one filled. Returns the index
// of the choice clicked, or -1.
int Segmented(const char* id, std::initializer_list<const char*> choices, int selected, ImU32 fill)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 whole(ImGui::GetContentRegionAvail().x, S(30));
    const ImVec2 size(whole.x / static_cast<float>(choices.size()), whole.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + whole, kInset, S(7));
    draw->AddRect(start, start + whole, kBorder, S(7));
    int clicked = -1;
    int index = 0;
    ImGui::PushID(id);
    PushSize(13.5f);
    for (const char* choice : choices)
    {
        const ImVec2 min = start + ImVec2(size.x * index, 0);
        ImGui::SetCursorScreenPos(min);
        if (ImGui::InvisibleButton(choice, size, ImGuiButtonFlags_EnableNav))
            clicked = index;
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        if (index == selected)
            draw->AddRectFilled(min + ImVec2(S(3), S(3)), min + size - ImVec2(S(3), S(3)), fill, S(5));
        draw->AddText(min + (size - ImGui::CalcTextSize(choice)) * 0.5f, index == selected ? IM_COL32_WHITE : hovered ? kText : kDim, choice);
        ++index;
    }
    ImGui::PopFont();
    ImGui::PopID();
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

void ProgressBar(float fraction)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(4));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + size, kBorder, S(2));
    draw->AddRectFilled(start, start + ImVec2(size.x * std::clamp(fraction, 0.0f, 1.0f), size.y), kAccent, S(2));
    ImGui::Dummy(size);
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

// A camera: the body, the bump on top and the lens.
void CameraIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 min = center + ImVec2(-size * 0.5f, -size * 0.3f);
    const ImVec2 max = center + ImVec2(size * 0.5f, size * 0.4f);
    draw->AddRect(min, max, color, S(2), 0, S(1.5f));
    draw->AddRectFilled(ImVec2(center.x - size * 0.2f, min.y - size * 0.15f), ImVec2(center.x + size * 0.2f, min.y), color, S(1));
    draw->AddCircle(ImVec2(center.x, center.y + size * 0.05f), size * 0.2f, color, 0, S(1.5f));
}

// Points down when open and right when closed.
void Chevron(ImDrawList* draw, ImVec2 center, bool open, ImU32 color)
{
    const float r = S(3.5f);
    if (open)
    {
        const ImVec2 points[] = { center + ImVec2(-r, -r / 2), center + ImVec2(0, r / 2), center + ImVec2(r, -r / 2) };
        draw->AddPolyline(points, 3, color, 0, S(1.6f));
    }
    else
    {
        const ImVec2 points[] = { center + ImVec2(-r / 2, -r), center + ImVec2(r / 2, 0), center + ImVec2(-r / 2, r) };
        draw->AddPolyline(points, 3, color, 0, S(1.6f));
    }
}

// The colors of the logo's ring, side by side.
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

// A check, exclamation mark or cross in a tinted circle, for the messages.
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

// The first letter of a name, which stands in for a missing icon.
std::string Initial(const std::string& name)
{
    if (name.empty())
        return "?";
    size_t length = 1;
    while (length < name.size() && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80)
        ++length;
    return length == 1 ? std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(name[0])))) : name.substr(0, length);
}

// A dialog in the middle of the window, opened when open is set. Call EndDialog when this returns true.
bool BeginDialog(const char* id, bool& open, float width)
{
    if (open)
    {
        ImGui::OpenPopup(id);
        open = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(S(width), io.DisplaySize.x - S(16)), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
        return true;
    ImGui::PopStyleVar();
    return false;
}

void EndDialog()
{
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// The dialog's title and the line below it.
void DialogText(const std::string& title, const std::string& detail)
{
    Text(title, kText, 16.5f);
    if (!detail.empty())
        Text(detail, kDim, 13.5f);
}

// The dialog's buttons, on the right with the last one primary. Returns the index of the one clicked, or -1.
int DialogButtons(std::initializer_list<const char*> labels)
{
    ImGui::Dummy(ImVec2(0, S(2)));
    const float buttonWidth = S(100);
    const int count = static_cast<int>(labels.size());
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(20) - buttonWidth * count - S(8) * (count - 1));
    int clicked = -1;
    int index = 0;
    for (const char* label : labels)
    {
        if (index)
            ImGui::SameLine(0, S(8));
        if (Button(label, ImVec2(buttonWidth, S(32)), index == count - 1))
            clicked = index;
        ++index;
    }
    return clicked;
}

// Launcher

// Where the launcher's content is drawn. It scrolls, so places are given from its top left corner.
struct Page
{
    ImDrawList* draw;
    ImVec2 origin;
    float left;
    float right;

    ImVec2 At(float x, float y) const
    {
        return origin + ImVec2(x, y);
    }
};

// Picking a window either saves its game or uses the window until Detect automatically.
enum class Picker
{
    Add,
    Session,
};

// A removed game, which can be put back for a few seconds.
struct Removed
{
    AutoGame game;
    size_t index = 0; // where it was in the list
    double at = 0;
};

struct LauncherState
{
    Picker picker = Picker::Add;
    bool openPicker = false;
    // The executable's name for each window the picker lists.
    std::map<platform::WindowId, std::string> pickerDetails;
    std::optional<Removed> removed;
    // Whether the picked window is still open, asked now and then.
    bool selectedOpen = true;
    double selectedChecked = -10;
    double picturesChecked = 0;
    // The size last asked of the window manager, which is free to give another.
    int askedWidth = 0;
    int askedHeight = 0;
};
LauncherState launcher;

// One line of text between left and right, cut off at right, centered up and down around middle.
void Line(ImDrawList* draw, float left, float right, float middle, const std::string& text, ImU32 color, float size, bool bold = false,
          bool centered = false)
{
    PushSize(size, bold);
    const ImVec2 measured = ImGui::CalcTextSize(text.c_str());
    const float x = centered ? left + (right - left - measured.x) / 2 : left;
    draw->PushClipRect(ImVec2(left, middle - measured.y), ImVec2(right, middle + measured.y), true);
    draw->AddText(ImVec2(x, middle - measured.y / 2), color, text.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
}

// Text wrapped between position and right. Returns its height.
float Paragraph(ImDrawList* draw, ImVec2 position, float right, const std::string& text, ImU32 color, float size, bool bold = false)
{
    PushSize(size, bold);
    const float wrap = right - position.x;
    const float height = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).y;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, color, text.c_str(), nullptr, wrap);
    ImGui::PopFont();
    return height;
}

float ButtonWidth(const char* label, bool bold = false)
{
    return Measure(label, kBody, bold).x + S(28);
}

// A button at a place. Returns true when clicked.
bool PlacedButton(const char* label, ImVec2 min, ImVec2 size, bool primary = false)
{
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (primary)
        draw->AddRectFilled(min, min + size, hovered ? kAccentHover : kAccent, S(8));
    else
    {
        draw->AddRectFilled(min, min + size, hovered ? kBorder : kCardHover, S(8));
        draw->AddRect(min, min + size, hovered ? kBorderStrong : kBorder, S(8));
    }
    Line(draw, min.x, min.x + size.x, min.y + size.y / 2, label, primary ? IM_COL32_WHITE : kText, kBody, primary, true);
    return clicked;
}

// Text to click at a place, as high as height. Returns true when clicked.
bool PlacedLink(const char* label, ImVec2 position, float height, float size = kBody)
{
    const float width = Measure(label, size).x;
    ImGui::SetCursorScreenPos(position);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    Line(ImGui::GetWindowDrawList(), position.x, position.x + width + 1, position.y + height / 2, label, hovered ? kText : kAccentHover, size);
    return clicked;
}

void RoundedCard(ImDrawList* draw, ImVec2 min, ImVec2 max)
{
    draw->AddRectFilled(min, max, kCard, S(10));
    draw->AddRect(min, max, kBorder, S(10));
}

// A card as tall as what is in it, laid out by Dear ImGui. Returns its height from EndCard.
void BeginCard(const char* id, const Page& page, float y, ImU32 fill = kCard, ImU32 border = kBorder)
{
    ImGui::SetCursorScreenPos(page.At(page.left, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, fill);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(16)));
    ImGui::BeginChild(id, ImVec2(page.right - page.left, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

float EndCard()
{
    ImGui::EndChild();
    return ImGui::GetItemRectSize().y;
}

// A game's saved icon, or its name's first letter on a tile.
void GameIcon(ImDrawList* draw, ImVec2 min, float size, const std::string& name, float letterSize)
{
    const ImVec2 max = min + ImVec2(size, size);
    const std::string folder = FolderName(name);
    if (const ImTextureID logo = folder.empty() ? ImTextureID_Invalid : PictureTexture((PresetsDirectory() / folder).string(), size))
    {
        draw->AddImageRounded(ImTextureRef(logo), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(6));
        return;
    }
    draw->AddRectFilled(min, max, kCardHover, S(8));
    draw->AddRect(min, max, kBorder, S(8));
    Line(draw, min.x, max.x, (min.y + max.y) / 2, Initial(name), kText, letterSize, true, true);
}

void LauncherHeader(const Page& page, float& y)
{
    const float logo = std::round(S(44));
    const ImVec2 corner = page.At(page.left, y);
    if (const ImTextureID texture = PictureTexture(kLogoPicture, logo))
        page.draw->AddImage(ImTextureRef(texture), corner, corner + ImVec2(logo, logo));
    const float textX = corner.x + logo + S(14);
    PushSize(kTitle, true);
    page.draw->AddText(ImVec2(textX, corner.y + S(1)), kText, "Unishade");
    ImGui::PopFont();
    PushSize(kBody);
    page.draw->AddText(ImVec2(textX, corner.y + S(26)), kDim, "Version " UNISHADE_VERSION);
    ImGui::PopFont();
    y += S(44) + S(22);
}

void PermissionCard(const Page& page, float& y)
{
    if (platform::HasCapturePermission())
        return;
    BeginCard("permission", page, y);
    Text("Unishade needs to record the screen", kText, kSemibold, 0, true);
    Text("macOS asks before a program can copy another program's window. Allow Unishade under Privacy & Security, Screen & System Audio "
         "Recording, then open Unishade again.",
         kDim, kBody);
    ImGui::Dummy(ImVec2(0, S(2)));
    PushSize(kBody, true);
    if (Button("Allow screen recording", ImVec2(0, S(30)), true))
        platform::RequestCapturePermission();
    ImGui::PopFont();
    y += EndCard() + S(14);
}

// The state as a dot on the game's icon, or alone while there is no game.
void StatusIcon(const Page& page, ImVec2 min, float size, const std::string& name, ImU32 color)
{
    ImDrawList* draw = page.draw;
    if (name.empty())
    {
        const ImVec2 center = min + ImVec2(size, size) * 0.5f;
        draw->AddCircleFilled(center, S(10), (color & 0x00FFFFFF) | 0x40000000);
        draw->AddCircleFilled(center, S(5.5f), color);
        return;
    }
    GameIcon(draw, min, size, name, kTitle);
    const ImVec2 dot = min + ImVec2(size - S(3), size - S(3));
    draw->AddCircleFilled(dot, S(7.5f), kCard);
    draw->AddCircleFilled(dot, S(4.5f), color);
}

// The game, what is happening, and switching between detection and a picked window.
void StatusCard(App& app, const Page& page, float& y)
{
    const std::optional<platform::Window>& game = app.active ? app.active : app.selected;
    const std::string name = game ? game->title : std::string();
    std::string title, detail;
    ImU32 color = kAccent;
    bool windowClosed = false;
    if (!app.captureEnabled)
    {
        title = "Overlay off";
        detail = "Turn it back on with " + app.HotkeyText(kOverlayToggleHotkey) + ".";
        color = kDim;
    }
    else if (app.active)
    {
        title = "Running on " + app.active->title;
        detail = "Press " + app.HotkeyText(kEditModeHotkey) + " in the game to open the menu.";
        color = kSuccess;
    }
    else if (app.selected)
    {
        // Each check asks the system, so only a few times a second.
        if (glfwGetTime() - launcher.selectedChecked > 0.5)
        {
            launcher.selectedOpen = platform::WindowExists(*app.selected);
            launcher.selectedChecked = glfwGetTime();
        }
        windowClosed = !launcher.selectedOpen;
        title = "Waiting for " + app.selected->title;
        detail = windowClosed ? "Its window closed. Pick its new window." : "Return to the game to see the effects.";
    }
    else
    {
        title = "Waiting for a game";
        detail = std::any_of(app.autoGames.begin(), app.autoGames.end(), [](const AutoGame& saved) { return saved.enabled; })
                     ? "Open one of your games."
                     : "Open a game and choose Add game.";
    }

    std::vector<std::pair<const char*, bool>> buttons; // the label, and whether it opens the picker
    if (!app.selected || windowClosed)
        buttons.emplace_back("Pick a window", true);
    if (app.selected)
        buttons.emplace_back("Detect automatically", false);
    float buttonWidth = 0;
    for (const auto& [label, pick] : buttons)
        buttonWidth = std::max(buttonWidth, ButtonWidth(label));
    const float buttonsHeight = static_cast<float>(buttons.size()) * (S(30) + S(8)) - S(8);

    const float inner = S(18), icon = S(40);
    const float textLeft = page.left + inner + icon + S(14);
    const float textRight = page.right - inner - buttonWidth - S(16);
    const float wrap = textRight - textLeft;
    const bool loading = app.runtime.Loading();
    const auto [loaded, total] = app.runtime.LoadingProgress();
    const std::string progress = "Compiling effects " + std::to_string(loaded) + " of " + std::to_string(total);
    const float titleHeight = Measure(title, kSemibold, true, wrap).y;
    const float detailHeight = Measure(detail, kBody, false, wrap).y;
    const float errorHeight = app.lastCaptureError.empty() ? 0 : S(3) + Measure(app.lastCaptureError, kBody, false, wrap).y;
    const float progressHeight = loading ? S(8) + Measure(progress, kNote).y + S(5) + S(4) : 0;
    const float textHeight = titleHeight + S(3) + detailHeight + errorHeight + progressHeight;
    const float height = std::max({ icon, textHeight, buttonsHeight }) + inner * 2;
    RoundedCard(page.draw, page.At(page.left, y), page.At(page.right, y + height));
    const float middle = y + height / 2;
    StatusIcon(page, page.At(page.left + inner, middle - icon / 2), icon, name, color);

    float textY = middle - textHeight / 2;
    textY += Paragraph(page.draw, page.At(textLeft, textY), page.origin.x + textRight, title, kText, kSemibold, true) + S(3);
    textY += Paragraph(page.draw, page.At(textLeft, textY), page.origin.x + textRight, detail, kDim, kBody);
    if (!app.lastCaptureError.empty())
        textY += S(3) + Paragraph(page.draw, page.At(textLeft, textY + S(3)), page.origin.x + textRight, app.lastCaptureError, kWarning, kBody);
    if (loading)
    {
        textY += S(8);
        textY += Paragraph(page.draw, page.At(textLeft, textY), page.origin.x + textRight, progress, kDim, kNote) + S(5);
        const ImVec2 bar = page.At(textLeft, textY);
        page.draw->AddRectFilled(bar, bar + ImVec2(wrap, S(4)), kBorder, S(2));
        page.draw->AddRectFilled(bar, bar + ImVec2(wrap * (total ? float(loaded) / float(total) : 0.0f), S(4)), kAccent, S(2));
    }

    float buttonTop = middle - buttonsHeight / 2;
    for (const auto& [label, pick] : buttons)
    {
        if (PlacedButton(label, page.At(page.right - inner - buttonWidth, buttonTop), ImVec2(buttonWidth, S(30))))
        {
            if (pick)
            {
                launcher.picker = Picker::Session;
                launcher.openPicker = true;
            }
            else
                app.Select(std::nullopt);
        }
        buttonTop += S(30) + S(8);
    }
    y += height + S(14);
}

// Downloads the effects, which Setup does on Windows.
void EffectsSetupCard(App& app, const Page& page, float& y)
{
    const EffectSetup::State state = app.setup.Read();
    if (app.EffectsInstalled() && !state.running && !state.finished)
        return;
    const bool attention = !state.running && !state.finished;
    BeginCard("setup", page, y, attention ? Color(theme::kAccent, 0x22) : kCard, attention ? Color(theme::kAccent, 0x90) : kBorder);
    if (state.running)
    {
        Text("Installing effects", kText, kSemibold, 0, true);
        Text(state.status + (state.detail.empty() ? "" : "\n" + state.detail), kDim, kBody);
        ProgressBar(state.fraction);
        ImGui::Dummy(ImVec2(0, S(2)));
        PushSize(kBody);
        if (Button("Cancel", ImVec2(0, S(30))))
            app.setup.Cancel();
        ImGui::PopFont();
    }
    else if (state.finished)
    {
        Text(state.error.empty() ? "Effects are installed" : "Installing effects failed", kText, kSemibold, 0, true);
        Text(state.error.empty() ? state.status : state.error, kDim, kBody);
        for (const std::string& note : state.notes)
            Text("Skipped " + note, kDim, kBody);
        if (!state.error.empty())
        {
            ImGui::Dummy(ImVec2(0, S(2)));
            PushSize(kBody, true);
            if (Button("Try again", ImVec2(0, S(30)), true))
                app.setup.Start();
            ImGui::PopFont();
        }
    }
    else
    {
        Text("Effects are not installed yet", kText, kSemibold, 0, true);
        Text("Unishade runs ReShade's effects. Download every package from ReShade's official list and the presets made for Unishade into "
             "the data folder.",
             kDim, kBody);
        ImGui::Dummy(ImVec2(0, S(2)));
        PushSize(kBody, true);
        if (Button("Download effects and presets", ImVec2(0, S(30)), true))
            app.setup.Start();
        ImGui::PopFont();
    }
    y += EndCard() + S(14);
}

// A cross. Returns true when clicked.
bool RemoveButton(ImVec2 min, float size)
{
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton("remove", ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    Tooltip("Remove from list");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = min + ImVec2(size, size) * 0.5f;
    if (hovered)
        draw->AddCircleFilled(center, size / 2, Color(theme::kError, 0x24));
    const float s = S(4.5f);
    const ImU32 color = hovered ? kError : kDim;
    draw->AddLine(center + ImVec2(-s, -s), center + ImVec2(s, s), color, S(1.6f));
    draw->AddLine(center + ImVec2(s, -s), center + ImVec2(-s, s), color, S(1.6f));
    return clicked;
}

void UndoRemove(App& app)
{
    const Removed removed = std::move(*launcher.removed);
    launcher.removed.reset();
    // Unless it was added again meanwhile.
    if (std::any_of(app.autoGames.begin(), app.autoGames.end(),
                    [&](const AutoGame& game) { return !strcasecmp(game.executable.c_str(), removed.game.executable.c_str()); }))
        return;
    app.autoGames.insert(app.autoGames.begin() + static_cast<std::ptrdiff_t>(std::min(removed.index, app.autoGames.size())), removed.game);
    app.SaveGames();
}

void GamesSection(App& app, const Page& page, float& y)
{
    ImDrawList* draw = page.draw;
    y += S(12);
    const float addWidth = ButtonWidth("Add game", true);
    Line(draw, page.origin.x + page.left, page.origin.x + page.right - addWidth - S(12), page.origin.y + y + S(15), "Games", kText, kSemibold, true);
    if (PlacedButton("Add game", page.At(page.right - addWidth, y), ImVec2(addWidth, S(30)), true))
    {
        launcher.picker = Picker::Add;
        launcher.openPicker = true;
    }
    y += S(30) + S(12);

    if (launcher.removed && glfwGetTime() - launcher.removed->at > kUndoSeconds)
        launcher.removed.reset();
    // A removed game keeps its row while it can be put back, so a second click does not land on the next game.
    const size_t removedRow = launcher.removed ? std::min(launcher.removed->index, app.autoGames.size()) : SIZE_MAX;
    const size_t rows = app.autoGames.size() + (launcher.removed ? 1 : 0);
    const float rowHeight = S(56);
    const float listTop = y;
    RoundedCard(draw, page.At(page.left, y), page.At(page.right, y + static_cast<float>(std::max<size_t>(rows, 1)) * rowHeight));
    if (!rows)
        Line(draw, page.origin.x + page.left + S(18), page.origin.x + page.right - S(18), page.origin.y + y + rowHeight / 2,
             "No games yet. Open one and click Add game.", kDim, kBody);

    std::optional<size_t> toggled, removedNow;
    bool undo = false;
    for (size_t row = 0; row < rows; ++row)
    {
        const float top = listTop + static_cast<float>(row) * rowHeight;
        const float middle = top + rowHeight / 2;
        if (row)
            draw->AddLine(page.At(page.left + S(14), top), page.At(page.right - S(14), top), kBorder);
        if (row == removedRow)
        {
            // Undo follows the text, away from where the remove button was.
            const std::string text = "Removed " + launcher.removed->game.name + ".";
            const float left = page.left + S(14);
            const float undoWidth = Measure("Undo", kBody).x;
            const float textEnd = left + std::min(Measure(text, kBody).x, page.right - S(64) - undoWidth - S(10) - left);
            Line(draw, page.origin.x + left, page.origin.x + textEnd, page.origin.y + middle, text, kDim, kBody);
            undo |= PlacedLink("Undo", page.At(textEnd + S(10), middle - S(10)), S(20));
            continue;
        }
        const size_t index = row > removedRow ? row - 1 : row;
        const AutoGame& game = app.autoGames[index];
        ImGui::PushID(static_cast<int>(index));
        const float removeLeft = page.right - S(12) - S(28);
        if (RemoveButton(page.At(removeLeft, middle - S(14)), S(28)))
            removedNow = index;
        const float toggleLeft = removeLeft - S(10) - S(36);
        ImGui::SetCursorScreenPos(page.At(toggleLeft, middle - S(10)));
        if (Switch("enabled", game.enabled, ImVec2(S(36), S(20))))
            toggled = index;
        float nameRight = toggleLeft - S(14);
        if (game.enabled && app.active && MatchesProcess(game, app.activeExecutable, app.activeCommand))
        {
            const float badgeWidth = Measure("Running", kNote).x + S(18);
            const ImVec2 badge = page.At(nameRight - badgeWidth, middle - S(11));
            draw->AddRectFilled(badge, badge + ImVec2(badgeWidth, S(22)), Color(theme::kSuccess, 0x26), S(11));
            Line(draw, badge.x, badge.x + badgeWidth, page.origin.y + middle, "Running", kSuccess, kNote, false, true);
            nameRight -= badgeWidth + S(12);
        }
        const float iconSize = std::round(S(32));
        const ImVec2 icon = page.At(page.left + S(14), middle - iconSize / 2);
        GameIcon(draw, icon, iconSize, game.name, kSemibold);
        // Turned-off games are faded.
        if (!game.enabled)
            draw->AddRectFilled(icon, icon + ImVec2(iconSize, iconSize), Color(theme::kCard, 0xA0));
        const float nameLeft = icon.x + iconSize + S(12);
        Line(draw, nameLeft, page.origin.x + nameRight, page.origin.y + middle - S(10), game.name, game.enabled ? kText : kDim, kBody, true);
        Line(draw, nameLeft, page.origin.x + nameRight, page.origin.y + middle + S(10), game.executable, kDim, kNote);
        ImGui::PopID();
    }
    if (toggled)
    {
        app.autoGames[*toggled].enabled = !app.autoGames[*toggled].enabled;
        app.SaveGames();
    }
    if (removedNow)
    {
        launcher.removed = Removed{ app.autoGames[*removedNow], *removedNow, glfwGetTime() };
        app.autoGames.erase(app.autoGames.begin() + static_cast<std::ptrdiff_t>(*removedNow));
        app.SaveGames();
    }
    if (undo)
        UndoRemove(app);
    y = listTop + static_cast<float>(std::max<size_t>(rows, 1)) * rowHeight + S(26);
}

// Warnings and errors, until dismissed.
void MessagesSection(const Page& page, float& y)
{
    const std::vector<Notice> notices = Notices();
    if (notices.empty())
        return;
    const float dismissWidth = Measure("Dismiss", kBody).x;
    Line(page.draw, page.origin.x + page.left, page.origin.x + page.right - dismissWidth - S(12), page.origin.y + y + S(11), "Messages", kText, kSemibold,
         true);
    if (PlacedLink("Dismiss", page.At(page.right - dismissWidth, y), S(22)))
        ClearNotices();
    y += S(22) + S(10);
    for (auto notice = notices.rbegin(); notice != notices.rend(); ++notice)
    {
        const float left = page.left + S(26);
        NoticeIcon(page.draw, page.At(left - S(18), y + S(9)), notice->level);
        y += std::max(Paragraph(page.draw, page.At(left, y), page.origin.x + page.right, notice->text, kText, kBody), S(18)) + S(8);
    }
    y += S(8);
}

float LauncherFooterHeight()
{
    return S(14) + S(24) + S(12) + S(20) + S(16);
}

void LauncherFooter(App& app, float top, float width)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(0, top), ImVec2(width, top), kBorder);
    const float pad = S(28);
    const float settingTop = top + S(14);

    // The switch with its label after it. There is no notification area icon to turn the effects off from.
    const char* label = "Show effects";
    const float labelWidth = Measure(label, kBody).x;
    ImGui::SetCursorScreenPos(ImVec2(pad, settingTop));
    if (ImGui::InvisibleButton(label, ImVec2(S(36) + S(10) + labelWidth, S(24)), ImGuiButtonFlags_EnableNav))
        app.ToggleOverlay();
    HandOnHover();
    DrawSwitch(draw, ImVec2(pad, settingTop + S(2)), ImVec2(S(36), S(20)), app.captureEnabled, ImGui::IsItemHovered());
    Line(draw, pad + S(36) + S(10), width, settingTop + S(12), label, kText, kBody);

    const float linkY = settingTop + S(24) + S(12);
    float x = pad;
    const auto link = [&](const char* text) {
        const bool clicked = PlacedLink(text, ImVec2(x, linkY), S(20));
        x += Measure(text, kBody).x + S(20);
        return clicked;
    };
    if (link("Open log"))
        platform::Open(LogPath().string());
    if (link("Get help on Discord"))
        platform::Open(kHelpUrl);
    if (link("Data folder"))
        platform::Open(DataDirectory().string());
    if (link("Screenshots"))
    {
        const fs::path folder = ScreenshotDirectory();
        std::error_code error;
        fs::create_directories(folder, error);
        platform::Open(folder.string());
    }
    if (link("Docs"))
        platform::Open("https://unishade.me/docs/");
}

void UseWindow(App& app, const platform::Window& window)
{
    if (launcher.picker == Picker::Add)
    {
        if (!AddAutoGame(app.autoGames, window))
        {
            Report(LogLevel::Warning, "%s has closed.", window.title.c_str());
            return;
        }
        // Detection follows the game in front, which is the one just added.
        app.Select(std::nullopt);
        app.SaveGames();
    }
    else
        app.Select(window);
    platform::Activate(window);
}

// The open windows, over the launcher.
void PickerDialog(App& app)
{
    if (launcher.openPicker)
    {
        ImGui::OpenPopup("##picker");
        launcher.openPicker = false;
        launcher.pickerDetails.clear();
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(S(kPickerWidth), io.DisplaySize.x - S(32));
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, std::min(S(kPickerMaxHeight), io.DisplaySize.y - S(32))));
    if (!ImGui::BeginPopupModal("##picker", nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const Page page{ ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), S(20), width - S(20) };
    float y = S(18);
    Line(page.draw, page.origin.x + page.left, page.origin.x + page.right, page.origin.y + y + S(10),
         launcher.picker == Picker::Add ? "Add a game" : "Pick a window", kText, kSemibold, true);
    y += S(20) + S(8);
    y += Paragraph(page.draw, page.At(page.left, y), page.origin.x + page.right,
                   launcher.picker == Picker::Add ? "Pick your game's window. If it isn't here, open the game first."
                                                  : "Unishade uses this window until you click Detect automatically.",
                   kDim, kBody) + S(14);

    const std::vector<platform::Window>& windows = app.Windows();
    const float rowHeight = S(52);
    const float listTop = y;
    RoundedCard(page.draw, page.At(page.left, y), page.At(page.right, y + static_cast<float>(std::max<size_t>(windows.size(), 1)) * rowHeight));
    if (windows.empty())
        Line(page.draw, page.origin.x + page.left + S(18), page.origin.x + page.right, page.origin.y + y + rowHeight / 2, "No open windows.", kDim, kBody);
    std::optional<platform::Window> chosen;
    for (size_t i = 0; i < windows.size(); ++i)
    {
        const platform::Window& window = windows[i];
        const float top = listTop + static_cast<float>(i) * rowHeight;
        const float middle = top + rowHeight / 2;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(page.At(page.left, top));
        if (ImGui::InvisibleButton("window", ImVec2(page.right - page.left, rowHeight), ImGuiButtonFlags_EnableNav))
            chosen = window;
        HandOnHover();
        if (ImGui::IsItemHovered())
            page.draw->AddRectFilled(page.At(page.left + S(4), top + S(4)), page.At(page.right - S(4), top + rowHeight - S(4)), kCardHover, S(8));
        else if (i)
            page.draw->AddLine(page.At(page.left + S(14), top), page.At(page.right - S(14), top), kBorder);
        auto detail = launcher.pickerDetails.find(window.id);
        if (detail == launcher.pickerDetails.end())
            detail = launcher.pickerDetails.emplace(window.id, fs::path(platform::ProcessExecutable(window.pid)).filename().string()).first;
        const float iconSize = std::round(S(32));
        const ImVec2 icon = page.At(page.left + S(14), middle - iconSize / 2);
        page.draw->AddRectFilled(icon, icon + ImVec2(iconSize, iconSize), kCardHover, S(8));
        page.draw->AddRect(icon, icon + ImVec2(iconSize, iconSize), kBorder, S(8));
        Line(page.draw, icon.x, icon.x + iconSize, page.origin.y + middle, Initial(window.title), kText, kSemibold, true, true);
        const float nameLeft = icon.x + iconSize + S(12);
        const float nameRight = page.origin.x + page.right - S(14);
        Line(page.draw, nameLeft, nameRight, page.origin.y + middle - S(10), window.title, kText, kBody, true);
        Line(page.draw, nameLeft, nameRight, page.origin.y + middle + S(10), detail->second, kDim, kNote);
        ImGui::PopID();
    }
    y = listTop + static_cast<float>(std::max<size_t>(windows.size(), 1)) * rowHeight + S(20);
    ImGui::SetCursorScreenPos(page.At(0, y));
    ImGui::Dummy(ImVec2(width, 0));

    if (chosen || ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered()))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (chosen)
        UseWindow(app, *chosen);
}

// Sizes the launcher's window to its content, up to the screen. A window manager that keeps another size is asked
// once.
void FitLauncher(App& app, float contentHeight)
{
    GLFWwindow* window = app.launcher.window;
    const ImVec2 current = ImGui::GetIO().DisplaySize;
    const int width = static_cast<int>(std::lround(S(kLauncherWidth)));
    int height = static_cast<int>(std::ceil(std::max(contentHeight, S(kLauncherMinHeight))));
    const auto fits = [&] { return width == static_cast<int>(current.x) && height == static_cast<int>(current.y); };
    if (fits())
        return;

    // The part of the window's monitor that is free of panels, or of the first monitor when the window is on none.
    int windowX = 0, windowY = 0, left = 0, top = 0, right = 0, bottom = 0, workTop = 0, workHeight = 0, count = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowFrameSize(window, &left, &top, &right, &bottom);
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i)
    {
        int x = 0, y = 0, monitorWidth = 0, monitorHeight = 0;
        glfwGetMonitorWorkarea(monitors[i], &x, &y, &monitorWidth, &monitorHeight);
        const int middle = windowX + static_cast<int>(current.x) / 2;
        const bool inside = middle >= x && middle < x + monitorWidth && windowY >= y && windowY < y + monitorHeight;
        if (i == 0 || inside)
        {
            workTop = y;
            workHeight = monitorHeight;
        }
        if (inside)
            break;
    }
    if (workHeight > top + bottom)
        height = std::min(height, workHeight - top - bottom);
    if (fits() || (width == launcher.askedWidth && height == launcher.askedHeight))
        return;
    launcher.askedWidth = width;
    launcher.askedHeight = height;
    glfwSetWindowSize(window, width, height);
    // Only moved when it grows past the bottom of the screen, so a window placed partly off the screen on purpose
    // stays there.
    if (workHeight > 0 && height > static_cast<int>(current.y) && windowY + height + bottom > workTop + workHeight)
        glfwSetWindowPos(window, windowX, std::max(workTop + top, workTop + workHeight - height - bottom));
}

// Menu

enum class Tab
{
    Presets,
    Effects,
    Settings,
    Status,
};

// The parts of the Settings tab, in the order their buttons show.
enum class SettingsPage
{
    General,
    Shortcuts,
};

enum class NameAction
{
    New,
    Duplicate,
    SaveAsNew,
    NewFolder,
};

struct MenuState
{
    Tab tab = Tab::Presets;
    SettingsPage settingsPage = SettingsPage::General;
    char search[128] = {};
    char presetSearch[128] = {};
    // The effect whose settings show, as presets name it.
    std::string expanded;
    bool showAll = false;
    // The effects listed under Active since the menu opened or the preset changed, as presets name them.
    std::set<std::string> active;
    fs::path activePreset;
    std::vector<PresetFolder> folders;
    double presetsListed = -10;
    fs::path pendingPreset; // waiting for an answer about unsaved changes
    bool openUnsavedPopup = false;
    bool openNamePopup = false;
    NameAction nameAction = NameAction::New;
    fs::path nameTarget;
    char name[128] = {};
    std::string nameError;
    bool openConfirmPopup = false;
    std::string confirmEffect; // the file whose settings Reset all asks about
    int recording = -1;        // the shortcut being recorded
    std::string shortcutError;
    std::vector<Notice> notices; // read once a frame
    std::string shownErrors;     // the effect file whose compiler errors show
};
MenuState menu;

// Settings lists the shortcuts in the order of kShortcuts.
constexpr const char* kShortcutDescriptions[] = {
    "Press it again, or Escape, to go back to the game.",
    "Shows the game without effects and stops capturing it.",
    "Shows the game without effects for as long as you hold it.",
    "Saves what you see, without the menu.",
    "Saves the same moment with and without effects.",
    "Switches to the next preset in the Presets tab.",
    "Switches to the preset before it.",
};
static_assert(std::size(kShortcutDescriptions) == std::size(kShortcuts));

void StopRecording(App& app)
{
    if (menu.recording >= 0)
        app.SuspendHotkeys(false);
    menu.recording = -1;
}

// Presets

void OpenNamePopup(NameAction action, const fs::path& target)
{
    menu.nameAction = action;
    menu.nameTarget = target;
    menu.nameError.clear();
    const std::string name = action == NameAction::New         ? "New preset"
                             : action == NameAction::NewFolder ? "New folder"
                                                               : target.stem().string() + " copy";
    snprintf(menu.name, sizeof(menu.name), "%s", name.c_str());
    menu.openNamePopup = true;
}

// Switches presets, or asks first when there are unsaved changes.
void SwitchTo(App& app, const fs::path& preset)
{
    if (app.SwitchPreset(preset, app.settings.autoSavePresets, false))
        return;
    menu.pendingPreset = preset;
    menu.openUnsavedPopup = true;
}

bool ApplyName(App& app)
{
    std::string name = menu.name;
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    menu.nameError.clear();
    const bool current = menu.nameTarget == app.runtime.PresetPath();
    if (menu.nameAction == NameAction::New)
        return app.NewPreset(name, false, menu.nameError);
    // The active preset is copied as it is on screen, unsaved changes included.
    if (menu.nameAction == NameAction::SaveAsNew || (menu.nameAction == NameAction::Duplicate && current))
        return app.NewPreset(name, true, menu.nameError);
    menu.nameError = PresetNameProblem(name);
    if (!menu.nameError.empty())
        return false;
    if (menu.nameAction == NameAction::NewFolder)
        return app.MovePreset(menu.nameTarget, PresetsDirectory() / name, menu.nameError);

    // A copy stays in the folder of the preset it comes from.
    const fs::path path = menu.nameTarget.parent_path() / (name + ".ini");
    std::error_code error;
    if (fs::exists(path, error))
    {
        menu.nameError = "A preset with that name already exists.";
        return false;
    }
    if (!fs::copy_file(menu.nameTarget, path, error))
    {
        menu.nameError = "Could not write the preset.";
        return false;
    }
    SwitchTo(app, path);
    return true;
}

void NameDialog(App& app)
{
    if (!BeginDialog("##name", menu.openNamePopup, 380))
        return;
    const char* titles[] = { "New preset", "Duplicate preset", "Save as new preset", "Move to a new folder" };
    const char* actions[] = { "Create", "Duplicate", "Save", "Move" };
    const int action = static_cast<int>(menu.nameAction);
    DialogText(titles[action], menu.nameAction == NameAction::New ? "Starts with every effect off." : "");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputText("##value", menu.name, sizeof(menu.name), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!menu.nameError.empty())
        Text(menu.nameError, kError, 13.5f);
    const int clicked = DialogButtons({ "Cancel", actions[action] });
    if ((clicked == 1 || enter) && ApplyName(app))
    {
        menu.presetsListed = -10;
        ImGui::CloseCurrentPopup();
    }
    if (clicked == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Asks what to do with unsaved changes when switching presets. The switch waits for the answer.
void UnsavedDialog(App& app)
{
    if (!BeginDialog("##unsaved", menu.openUnsavedPopup, 420))
        return;
    DialogText("Save your changes to " + app.runtime.PresetPath().stem().string() + "?", "Discarding goes back to how the preset was last saved.");
    const int clicked = DialogButtons({ "Discard", "Cancel", "Save" });
    if (clicked == 0)
        app.SwitchPreset(menu.pendingPreset, false, true);
    else if (clicked == 2)
        app.SwitchPreset(menu.pendingPreset, true, false);
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        menu.pendingPreset.clear();
        ImGui::CloseCurrentPopup();
    }
    EndDialog();
}

// The folders a preset can move to: the ones listed, then saved games that have no presets yet.
void MoveMenu(App& app, const fs::path& preset)
{
    std::vector<std::pair<std::string, fs::path>> targets;
    for (const PresetFolder& folder : menu.folders)
        targets.emplace_back(folder.name, folder.path);
    for (const AutoGame& game : app.autoGames)
    {
        const std::string name = FolderName(game.name);
        if (!name.empty() && std::none_of(targets.begin(), targets.end(), [&](const auto& target) {
                return !strcasecmp(target.second.filename().c_str(), name.c_str());
            }))
            targets.emplace_back(name, PresetsDirectory() / name);
    }
    for (const auto& [name, folder] : targets)
        if (folder != preset.parent_path() && ImGui::MenuItem(name.c_str()))
        {
            if (std::string error; !app.MovePreset(preset, folder, error))
                app.ShowToast(error);
            menu.presetsListed = -10;
        }
    ImGui::Separator();
    if (ImGui::MenuItem("New folder..."))
        OpenNamePopup(NameAction::NewFolder, preset);
}

// The preset's menu, opened from the three dots on its row.
void PresetActions(App& app, const fs::path& path, bool active)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("actions"))
    {
        if (ImGui::MenuItem("Duplicate"))
            OpenNamePopup(NameAction::Duplicate, path);
        if (ImGui::BeginMenu("Move to", !active))
        {
            MoveMenu(app, path);
            ImGui::EndMenu();
        }
        if (active)
        {
            ImGui::Separator();
            PushSize(12.5f);
            ImGui::TextDisabled("Switch to another preset to\nmove this one.");
            ImGui::PopFont();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void PresetRow(App& app, const fs::path& path, bool active)
{
    const std::string name = path.stem().string();
    ImGui::PushID(name.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(44));
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav) && !active;
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
        const ImVec2 tagText = ImGui::CalcTextSize(tag);
        const ImVec2 tagSize = tagText + ImVec2(S(14), S(6));
        const ImVec2 tagStart(dots.x - S(8) - tagSize.x, start.y + (size.y - tagSize.y) / 2);
        draw->AddRectFilled(tagStart, tagStart + tagSize, Color(theme::kAccent, 50), tagSize.y / 2);
        draw->AddText(tagStart + (tagSize - tagText) * 0.5f, kAccentHover, tag);
        ImGui::PopFont();
    }

    PresetActions(app, path, active);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    ImGui::PopID();
    if (clicked)
        SwitchTo(app, path);
}

// The folder's logo.png, such as a game's icon, or a stand-in: a globe for all games, a game's first letter, or a
// folder.
void FolderIcon(ImDrawList* draw, const PresetFolder& folder, ImVec2 min, float size)
{
    const ImVec2 max = min + ImVec2(size, size);
    if (const ImTextureID logo = PictureTexture(folder.path.string(), size))
    {
        draw->AddImageRounded(ImTextureRef(logo), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
        return;
    }
    const ImVec2 center = (min + max) * 0.5f;
    const float t = S(1.5f);
    if (folder.all)
    {
        const float r = size * 0.4f;
        draw->AddCircle(center, r, kDim, 0, t);
        draw->AddEllipse(center, ImVec2(r * 0.45f, r), kDim, 0, 0, t);
        draw->AddLine(center - ImVec2(r, 0), center + ImVec2(r, 0), kDim, t);
    }
    else if (folder.game)
    {
        draw->AddRectFilled(min, max, kBorder, S(5));
        const std::string letter = Initial(folder.name);
        PushSize(13);
        draw->AddText(center - ImGui::CalcTextSize(letter.c_str()) * 0.5f, kText, letter.c_str());
        ImGui::PopFont();
    }
    else
    {
        const ImVec2 body(min.x + size * 0.1f, min.y + size * 0.32f);
        draw->AddRectFilled(ImVec2(body.x, min.y + size * 0.2f), ImVec2(body.x + size * 0.35f, body.y + t), kDim, S(1.5f));
        draw->AddRect(body, ImVec2(max.x - size * 0.1f, max.y - size * 0.18f), kDim, S(2), 0, t);
    }
}

// A folder's logo, name and preset count. Clicking it opens or closes the folder.
bool FolderHeader(const PresetFolder& folder, bool open, size_t presets)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(34));
    const bool clicked = ImGui::InvisibleButton("folder", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(start, start + size, kCard, S(8));

    const float icon = std::round(S(22));
    const ImVec2 iconStart(std::round(start.x + S(6)), std::round(start.y + (size.y - icon) / 2));
    FolderIcon(draw, folder, iconStart, icon);

    const float chevron = start.x + size.x - S(20);
    PushSize(13);
    const std::string count = std::to_string(presets);
    const ImVec2 countSize = ImGui::CalcTextSize(count.c_str());
    const float countX = chevron - S(16) - countSize.x;
    draw->AddText(ImVec2(countX, start.y + (size.y - countSize.y) / 2), kDim, count.c_str());
    ImGui::PopFont();
    PushSize(14.5f);
    draw->PushClipRect(start, ImVec2(countX - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(iconStart.x + icon + S(10), start.y + (size.y - ImGui::GetFontSize()) / 2), kText, folder.name.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    Chevron(draw, ImVec2(chevron, start.y + size.y / 2), open, open || hovered ? kText : kDim);
    return clicked;
}

// Returns false when searching found nothing in the folder, which is then left out.
bool FolderSection(App& app, const PresetFolder& folder, const fs::path& current, const std::string& filter)
{
    // While searching, folders are open and show the presets that match, or all of them when the folder's name does.
    const bool searching = !filter.empty();
    const bool folderMatches = searching && Lowercase(folder.name).find(filter) != std::string::npos;
    std::vector<const fs::path*> shown;
    for (const fs::path& preset : folder.presets)
        if (!searching || folderMatches || Lowercase(preset.stem().string()).find(filter) != std::string::npos)
            shown.push_back(&preset);
    if (searching && shown.empty())
        return false;

    ImGui::PushID(folder.path.c_str());
    const bool open = searching || app.FolderOpen(folder);
    if (FolderHeader(folder, open, shown.size()) && !searching)
        app.folderOpen[folder.path.string()] = !open;
    if (open)
    {
        for (const fs::path* preset : shown)
            PresetRow(app, *preset, *preset == current);
        if (folder.presets.empty() && (folder.playing || (folder.all && app.game.empty())))
            Text("New presets go here.", kDim, 13.5f);
    }
    ImGui::PopID();
    return true;
}

void PresetsTab(App& app)
{
    const fs::path current = app.runtime.PresetPath();
    if (glfwGetTime() - menu.presetsListed > 1)
    {
        menu.folders = app.PresetFolders();
        menu.presetsListed = glfwGetTime();
        ForgetPictures([](const std::string& path) {
            return std::any_of(menu.folders.begin(), menu.folders.end(), [&](const PresetFolder& folder) { return folder.path.string() == path; });
        });
    }

    if (Button("New preset", ImVec2(S(130), S(32)), true))
        OpenNamePopup(NameAction::New, {});
    if (!app.settings.autoSavePresets)
    {
        ImGui::SameLine(0, S(8));
        if (Button("Save as new", ImVec2(S(120), S(32))))
            OpenNamePopup(NameAction::SaveAsNew, current);
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##presetsearch", "Search presets", menu.presetSearch, sizeof(menu.presetSearch));

    const std::string filter = Lowercase(menu.presetSearch);
    bool any = false;
    for (const PresetFolder& folder : menu.folders)
        if (FolderSection(app, folder, current, filter))
            any = true;
    if (!any && !filter.empty())
        Text("No presets match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    Text(app.settings.autoSavePresets ? "Changes save to the active preset as you make them."
                                      : "Changes apply right away. Save them with the icon at the top.",
         kDim, 13);
    PushSize(13.5f);
    if (Link("Open presets folder"))
        platform::Open(PresetsDirectory().string());
    if (!app.settings.autoSavePresets && app.runtime.Dirty())
    {
        ImGui::SameLine(0, S(16));
        if (Link("Discard changes", kDim))
            app.SwitchPreset(current, false, true);
    }
    ImGui::PopFont();
}

// Effects

// Draws the control for one variable of an effect. Returns true when its value changed.
bool DrawParameter(App& app, fx::Effect& effect, const fx::Uniform& uniform)
{
    const reshadefx::type& type = uniform.type;
    const int components = static_cast<int>(type.components());
    if (!uniform.text.empty())
        Text(uniform.text, kDim, 13.5f);
    if (type.is_array() || type.is_matrix() || components > 4)
        return false;

    bool changed = false;
    ImGui::PushID(uniform.name.c_str());
    const float x = ImGui::GetCursorPosX();
    const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
    // One group for the label and the control, so effects can tell which of their variables is in use or under the
    // cursor.
    ImGui::BeginGroup();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x + labelWidth - S(10));
    ImGui::PushStyleColor(ImGuiCol_Text, uniform.tooltip.empty() ? kText : Color(0xD8D9E6));
    ImGui::TextUnformatted(uniform.label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    Tooltip(uniform.tooltip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x + labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    const bool bounded = uniform.min > std::numeric_limits<float>::lowest() && uniform.max < std::numeric_limits<float>::max();
    if (type.is_boolean())
    {
        int value = 0;
        app.runtime.GetValue(effect, uniform, &value, 1);
        bool on = value != 0;
        if ((changed = ImGui::Checkbox("##value", &on)))
        {
            value = on;
            app.runtime.SetValue(effect, uniform, &value, 1);
        }
    }
    else if (!uniform.items.empty() && !type.is_floating_point() && components == 1)
    {
        int value = 0;
        app.runtime.GetValue(effect, uniform, &value, 1);
        if ((changed = ImGui::Combo("##value", &value, uniform.items.c_str())))
            app.runtime.SetValue(effect, uniform, &value, 1);
    }
    else if (uniform.uiType == "color" && type.is_floating_point() && components >= 3)
    {
        float value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        changed = components == 3 ? ImGui::ColorEdit3("##value", value) : ImGui::ColorEdit4("##value", value, ImGuiColorEditFlags_AlphaBar);
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    else if (type.is_floating_point())
    {
        float value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        if (uniform.uiType == "slider" && bounded)
            changed = ImGui::SliderScalarN("##value", ImGuiDataType_Float, value, components, &uniform.min, &uniform.max, "%.3f");
        else if (uniform.uiType == "input")
            changed = ImGui::InputScalarN("##value", ImGuiDataType_Float, value, components);
        else
            changed = ImGui::DragScalarN("##value", ImGuiDataType_Float, value, components, std::max(uniform.step, 0.0001f),
                                         bounded ? &uniform.min : nullptr, bounded ? &uniform.max : nullptr, "%.3f");
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    else
    {
        int value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        const int low = bounded ? static_cast<int>(uniform.min) : INT_MIN, high = bounded ? static_cast<int>(uniform.max) : INT_MAX;
        if (uniform.uiType == "slider" && bounded)
            changed = ImGui::SliderScalarN("##value", ImGuiDataType_S32, value, components, &low, &high);
        else if (uniform.uiType == "input")
            changed = ImGui::InputScalarN("##value", ImGuiDataType_S32, value, components);
        else
            changed = ImGui::DragScalarN("##value", ImGuiDataType_S32, value, components, std::max(uniform.step, 0.2f), bounded ? &low : nullptr,
                                         bounded ? &high : nullptr);
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    if (ImGui::BeginPopupContextItem("reset"))
    {
        if (ImGui::MenuItem("Reset to default"))
        {
            app.runtime.ResetValue(effect, uniform);
            changed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    if (ImGui::IsItemActive())
        app.menuActiveUniform = &uniform;
    if (ImGui::IsItemHovered())
        app.menuHoveredUniform = &uniform;
    ImGui::PopID();
    return changed;
}

// The preprocessor definitions the effect checks, such as quality levels. A change compiles the effects again, so it
// applies on Enter, as in ReShade's menu.
void DrawDefinitions(App& app, const fx::Effect& effect)
{
    if (effect.definitions.empty() || !ImGui::CollapsingHeader("Preprocessor definitions"))
        return;
    for (const auto& [name, compiled] : effect.definitions)
    {
        ImGui::PushID(name.c_str());
        const float x = ImGui::GetCursorPosX();
        const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(x + labelWidth - S(10));
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        ImGui::SetCursorPosX(x + labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Empty while the effect uses its own value, which shows as the hint.
        const std::string current = app.runtime.DefinitionValue(effect.file, name);
        char value[256]{};
        current.copy(value, sizeof(value) - 1);
        if (ImGui::InputTextWithHint("##value", compiled.c_str(), value, sizeof(value), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) &&
            current != value)
            app.runtime.SetDefinition(effect.file, name, value);
        if (ImGui::BeginPopupContextItem("reset"))
        {
            if (ImGui::MenuItem("Reset to default"))
                app.runtime.SetDefinition(effect.file, name, "");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

void DrawParameters(App& app, const fx::Technique& technique, fx::Effect& effect)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("settings", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    PushSize(13.5f);
    if (!technique.tooltip.empty())
        Text(technique.tooltip, kDim, 13.5f);

    // Grouped by category in the order they first appear, as in ReShade.
    std::vector<std::string> categories;
    for (const fx::Uniform& uniform : effect.uniforms)
        if (!uniform.hidden && std::find(categories.begin(), categories.end(), uniform.category) == categories.end())
            categories.push_back(uniform.category);
    if (categories.empty() && effect.definitions.empty())
        Text("This effect has no settings.", kDim, 13.5f);
    bool changed = false;
    for (const std::string& category : categories)
    {
        if (!category.empty() && !ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        for (const fx::Uniform& uniform : effect.uniforms)
            if (!uniform.hidden && uniform.category == category)
                changed |= DrawParameter(app, effect, uniform);
    }
    if (changed)
        app.runtime.SetDirty();
    if (!categories.empty())
    {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (Link("Reset all", kDim))
        {
            menu.confirmEffect = effect.file;
            menu.openConfirmPopup = true;
        }
        Tooltip("Right-click a setting to reset only that one.");
    }
    DrawDefinitions(app, effect);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Asks before resetting an effect's settings, which cannot be got back with auto-save on.
void ConfirmDialog(App& app)
{
    if (!BeginDialog("##confirm", menu.openConfirmPopup, 380))
        return;
    DialogText("Reset every setting of " + menu.confirmEffect + "?",
               app.settings.autoSavePresets ? "They go back to their defaults and the preset is saved." : "They go back to their defaults.");
    const int clicked = DialogButtons({ "Cancel", "Reset" });
    if (clicked == 1)
        for (fx::Effect& effect : app.runtime.Effects())
            if (effect.file == menu.confirmEffect)
            {
                for (const fx::Uniform& uniform : effect.uniforms)
                    if (uniform.source.empty())
                        app.runtime.ResetValue(effect, uniform);
                app.runtime.SetDirty();
            }
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Effects that are on can be dragged onto each other to change the order they run in, which from and to take.
void TechniqueRow(App& app, int index, int& moveFrom, int& moveTo)
{
    fx::Technique& technique = app.runtime.Techniques()[index];
    fx::Effect& effect = app.runtime.Effects()[technique.effect];
    const std::string key = fx::TechniqueKey(technique, effect);
    const std::string& label = technique.label.empty() ? technique.name : technique.label;
    const bool expanded = menu.expanded == key;
    ImGui::PushID(key.c_str());
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
        menu.expanded = expanded ? std::string() : key;
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    if (technique.enabled)
    {
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("technique", &index, sizeof(index));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("technique"))
            {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = index;
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || expanded)
        draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));

    ImGui::SetCursorScreenPos(start + ImVec2(S(10), (size.y - S(18)) / 2));
    if (Switch("on", technique.enabled))
        app.runtime.SetEnabled(index, !technique.enabled);

    const float chevron = start.x + size.x - S(20);
    PushSize(14.5f);
    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(chevron - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(52), textY), effect.gpuFailed ? kDim : kText, label.c_str());
    ImGui::PopFont();
    PushSize(12.5f);
    draw->AddText(ImVec2(start.x + S(52) + labelSize.x + S(8), textY + S(1.5f)), effect.gpuFailed ? kWarning : kDim,
                  effect.gpuFailed ? "failed" : effect.file.c_str());
    ImGui::PopFont();
    draw->PopClipRect();

    Chevron(draw, ImVec2(chevron, start.y + size.y / 2), expanded, expanded || hovered ? kText : kDim);

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    if (expanded)
        DrawParameters(app, technique, effect);
    ImGui::PopID();
}

void EffectsTab(App& app)
{
    std::vector<fx::Technique>& techniques = app.runtime.Techniques();
    const std::vector<fx::Effect>& effects = app.runtime.Effects();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search effects", menu.search, sizeof(menu.search));

    if (app.runtime.Loading())
    {
        const auto [loaded, total] = app.runtime.LoadingProgress();
        ImGui::Dummy(ImVec2(0, S(2)));
        Spinner(S(10));
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
        Text(techniques.empty() ? "Loading effects. The first time can take a minute."
                                : "Compiling effects " + std::to_string(loaded) + " of " + std::to_string(total),
             kDim, 14);
    }
    else if (techniques.empty())
    {
        ImGui::Dummy(ImVec2(0, S(6)));
        Text("No effects are installed. Install them from the launcher.", kDim, 14);
    }
    if (techniques.empty())
        return;

    const std::string filter = Lowercase(menu.search);
    const auto label = [](const fx::Technique& technique) -> const std::string& { return technique.label.empty() ? technique.name : technique.label; };
    const auto shown = [&](const fx::Technique& technique) {
        return !technique.hidden &&
               (filter.empty() || Lowercase(label(technique)).find(filter) != std::string::npos ||
                Lowercase(effects[technique.effect].file).find(filter) != std::string::npos);
    };

    // An effect turned off stays in Active until the menu closes, so it can be turned back on where it was.
    if (menu.activePreset != app.runtime.PresetPath())
    {
        menu.active.clear();
        menu.activePreset = app.runtime.PresetPath();
    }
    for (const fx::Technique& technique : techniques)
        if (technique.enabled)
            menu.active.insert(fx::TechniqueKey(technique, effects[technique.effect]));
    const auto active = [&](const fx::Technique& technique) { return menu.active.count(fx::TechniqueKey(technique, effects[technique.effect])) != 0; };

    const int count = static_cast<int>(techniques.size());
    int moveFrom = -1, moveTo = -1;
    Heading("ACTIVE");
    bool any = false;
    for (int i = 0; i < count; ++i)
        if (active(techniques[i]) && shown(techniques[i]))
        {
            TechniqueRow(app, i, moveFrom, moveTo);
            any = true;
        }
    if (!any)
        Text(filter.empty() ? "No effects are on. Pick a preset or turn effects on below." : "No active effects match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    // The rest by name.
    std::vector<int> others;
    for (int i = 0; i < count; ++i)
        if (!active(techniques[i]) && shown(techniques[i]))
            others.push_back(i);
    std::sort(others.begin(), others.end(), [&](int a, int b) { return strcasecmp(label(techniques[a]).c_str(), label(techniques[b]).c_str()) < 0; });
    const bool searching = !filter.empty();
    const std::string title = "ALL EFFECTS (" + std::to_string(others.size()) + ")";
    PushSize(12);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::SetNextItemOpen(searching || menu.showAll);
    const bool open = ImGui::TreeNodeEx((title + "###all").c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::IsItemToggledOpen() && !searching)
        menu.showAll = !menu.showAll;
    HandOnHover();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (open)
        for (int index : others)
            TechniqueRow(app, index, moveFrom, moveTo);

    if (moveFrom >= 0)
        app.runtime.MoveTechnique(moveFrom, moveTo);
}

// Settings

// While the menu waits for a shortcut, the next key pressed with any modifiers becomes it.
void RecordShortcut(App& app)
{
    if (menu.recording < 0)
        return;
    // Space would otherwise also press the control the keyboard is on.
    ImGui::SetNavCursorVisible(false);
    // Modifiers straight from GLFW: Dear ImGui swaps Cmd and Ctrl on macOS.
    GLFWwindow* window = app.overlay.window;
    const auto down = [window](int a, int b) { return glfwGetKey(window, a) == GLFW_PRESS || glfwGetKey(window, b) == GLFW_PRESS; };
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
    {
        if (!IsShortcutKey(static_cast<ImGuiKey>(key)) || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
            continue;
        Hotkey hotkey;
        hotkey.key = static_cast<ImGuiKey>(key);
        hotkey.modifiers = (down(GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL) ? kCtrl : 0u) | (down(GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT) ? kAlt : 0u) |
                           (down(GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT) ? kShift : 0u) |
                           (down(GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER) ? kSuper : 0u);
        InputHotkeys changed = app.settings.hotkeys;
        changed.*kShortcuts[menu.recording].member = hotkey;
        StopRecording(app);
        // Escape alone stops waiting.
        if (hotkey.key != ImGuiKey_Escape || hotkey.modifiers)
            app.ChangeHotkeys(changed, menu.shortcutError);
        break;
    }
}

void ShortcutRow(App& app, int index)
{
    const Shortcut& shortcut = kShortcuts[index];
    ImGui::PushID(index);
    const float x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = S(130);

    ImGui::BeginGroup();
    Text(shortcut.label, kText, 14.5f, x + width - buttonWidth - S(14));
    Text(kShortcutDescriptions[index], kDim, 13, x + width - buttonWidth - S(14));
    ImGui::EndGroup();
    const float bottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos(ImVec2(x + width - buttonWidth, top + S(2)));
    const bool recording = menu.recording == index;
    const std::string label = recording ? "Press keys..." : FormatHotkey(app.settings.hotkeys.*shortcut.member);
    ImGui::PushStyleColor(ImGuiCol_Border, recording ? kAccent : kBorderStrong);
    if (Button((label + "##key").c_str(), ImVec2(buttonWidth, S(32))))
    {
        if (recording)
            StopRecording(app);
        else
        {
            StopRecording(app);
            menu.recording = index;
            menu.shortcutError.clear();
            app.SuspendHotkeys(true);
        }
    }
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(x, std::max(bottom, top + S(40)) + S(6)));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

// A switch with its title and description beside it. Returns true when clicked.
bool SwitchRow(const char* id, bool on, const char* title, const char* description)
{
    const bool clicked = Switch(id, on);
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    Text(title, kText, 14.5f);
    Text(description, kDim, 13);
    ImGui::EndGroup();
    return clicked;
}

void GeneralSettings(App& app)
{
    Heading("PRESETS");
    if (SwitchRow("autosave", app.settings.autoSavePresets, "Save changes automatically",
                  "Turn off to try changes first and save them with the icon at the top."))
    {
        app.settings.autoSavePresets = !app.settings.autoSavePresets;
        // From here on every change saves as it happens, so changes that were waiting are saved too.
        if (app.settings.autoSavePresets && app.runtime.Dirty())
            app.runtime.SavePreset();
        SaveSettings(app.settings);
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    Heading("EFFECTS");
    for (const fs::path& path : app.settings.effectPaths)
        Text(path.string(), kDim, 13);
    ImGui::Dummy(ImVec2(0, S(2)));
    if (Button("Reload effects", ImVec2(S(130), S(32))))
        app.runtime.Reload();
    ImGui::SameLine(0, S(8));
    if (Button("Open data folder", ImVec2(S(150), S(32))))
        platform::Open(DataDirectory().string());
}

void ShortcutSettings(App& app)
{
    Text("Click a shortcut, then press the keys you want. They work right away.", kDim, 13);
    ImGui::Dummy(ImVec2(0, S(4)));
    for (int i = 0; i < static_cast<int>(std::size(kShortcuts)); ++i)
        ShortcutRow(app, i);
    if (!menu.shortcutError.empty())
        Text(menu.shortcutError, kError, 13.5f);
}

void SettingsTab(App& app)
{
    const int clicked = Segmented("page", { "General", "Shortcuts" }, static_cast<int>(menu.settingsPage), kBorder);
    if (clicked >= 0)
    {
        // A shortcut left waiting for keys on its page would take the next key pressed on another.
        StopRecording(app);
        menu.settingsPage = static_cast<SettingsPage>(clicked);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    if (menu.settingsPage == SettingsPage::General)
        GeneralSettings(app);
    else
        ShortcutSettings(app);
}

// Status

void StatusTab(App& app)
{
    const float x = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    Heading("STATUS");
    if (!menu.notices.empty())
    {
        // The log keeps them.
        PushSize(12);
        const char* dismiss = "Dismiss all";
        ImGui::SameLine(x + width - ImGui::CalcTextSize(dismiss).x);
        if (Link(dismiss, kDim))
            ClearNotices();
        ImGui::PopFont();
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PushSize(13.5f);
    for (auto notice = menu.notices.rbegin(); notice != menu.notices.rend(); ++notice)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        NoticeIcon(draw, start + ImVec2(S(8), ImGui::GetFontSize() / 2 + S(1)), notice->level);
        ImGui::SetCursorScreenPos(start + ImVec2(S(26), 0));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(notice->text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (menu.notices.empty())
        ImGui::TextDisabled("No messages.");
    ImGui::PopFont();

    std::vector<fx::Effect>& effects = app.runtime.Effects();
    const size_t failed = static_cast<size_t>(std::count_if(effects.begin(), effects.end(), [](const fx::Effect& effect) { return !effect.compiled; }));
    if (failed)
    {
        ImGui::Dummy(ImVec2(0, S(8)));
        Heading(("DID NOT COMPILE (" + std::to_string(failed) + ")").c_str());
        for (const fx::Effect& effect : effects)
        {
            if (effect.compiled)
                continue;
            ImGui::PushID(effect.file.c_str());
            const bool open = menu.shownErrors == effect.file;
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const ImVec2 size(ImGui::GetContentRegionAvail().x, S(34));
            if (ImGui::InvisibleButton("errors", size, ImGuiButtonFlags_EnableNav))
                menu.shownErrors = open ? std::string() : effect.file;
            const bool hovered = ImGui::IsItemHovered();
            HandOnHover();
            if (hovered || open)
                draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));
            PushSize(14.5f);
            draw->AddText(ImVec2(start.x + S(12), start.y + (size.y - ImGui::GetFontSize()) / 2), kText, effect.file.c_str());
            ImGui::PopFont();
            Chevron(draw, ImVec2(start.x + size.x - S(20), start.y + size.y / 2), open, open || hovered ? kText : kDim);
            if (open)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
                ImGui::BeginChild("text", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
                Text(effect.errors, kDim, 12.5f);
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0, S(4)));
    PushSize(13.5f);
    if (Link("Open log"))
        platform::Open(LogPath().string());
    ImGui::SameLine(0, S(16));
    if (Link("Get help on Discord"))
        platform::Open(kHelpUrl);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(6)));
    Text("Unishade " UNISHADE_VERSION ". Effects compile with ReShade's compiler by crosire.", kDim, 12.5f);
}

// Frame

void Header(App& app, ImVec2 origin, float width)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float logo = std::round(S(38));
    const ImVec2 logoPosition = origin + ImVec2(std::round(S(kPadding)), std::round(S(18)));
    if (const ImTextureID texture = PictureTexture(kLogoPicture, logo))
        draw->AddImage(ImTextureRef(texture), logoPosition, logoPosition + ImVec2(logo, logo));
    const float textX = S(kPadding) + logo + S(12);
    PushSize(16.5f);
    draw->AddText(origin + ImVec2(textX, S(17)), kText, "Unishade");
    ImGui::PopFont();
    // The right side holds the effects switch, its label and, with auto-save off, the save icon.
    const bool autoSave = app.settings.autoSavePresets;
    const bool unsaved = !autoSave && app.runtime.Dirty();
    PushSize(13);
    const ImVec2 labelSize = ImGui::CalcTextSize("Effects");
    const float switchX = width - S(kPadding) - S(32);
    const float labelX = switchX - S(8) - labelSize.x;
    const ImVec2 saveSize(S(30), S(30));
    const float saveX = labelX - S(14) - saveSize.x;

    const std::string preset = app.runtime.PresetPath().stem().string();
    const ImVec2 nameSize = ImGui::CalcTextSize(preset.c_str());
    draw->PushClipRect(origin, origin + ImVec2((autoSave ? labelX : saveX) - S(12), S(kHeader)), true);
    draw->AddText(origin + ImVec2(textX, S(39)), kDim, preset.c_str());
    if (unsaved)
        draw->AddCircleFilled(origin + ImVec2(textX + nameSize.x + S(7), S(39) + ImGui::GetFontSize() / 2 + S(1)), S(3), kWarning);
    draw->PopClipRect();
    ImGui::PopFont();

    if (!autoSave)
    {
        const ImVec2 start = origin + ImVec2(saveX, S(22));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton("save", saveSize, ImGuiButtonFlags_EnableNav) && unsaved)
            app.runtime.SavePreset();
        if (ImGui::IsItemHovered() && unsaved)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            draw->AddRectFilled(start, start + saveSize, kBorder, S(6));
        }
        Tooltip((unsaved ? "Save changes to " : "No unsaved changes in ") + preset);
        SaveIcon(draw, start + saveSize * 0.5f, S(14), unsaved ? kAccentHover : kBorderStrong);
    }

    // Effects on and off for everything.
    ImGui::SetCursorScreenPos(origin + ImVec2(switchX, S(28)));
    if (Switch("effects", app.effectsEnabled))
        app.effectsEnabled = !app.effectsEnabled;
    Tooltip(app.effectsEnabled ? "Turn all effects off" : "Turn effects back on");
    PushSize(13);
    draw->AddText(origin + ImVec2(labelX, S(28) + (S(18) - labelSize.y) / 2), kDim, "Effects");
    ImGui::PopFont();

    Rainbow(draw, origin + ImVec2(0, S(kHeader) - S(2)), origin + ImVec2(width, S(kHeader)));
}

void Tabs(ImVec2 origin, float width)
{
    constexpr std::pair<const char*, Tab> entries[] = {
        { "Presets", Tab::Presets }, { "Effects", Tab::Effects }, { "Settings", Tab::Settings }, { "Status", Tab::Status }
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tabWidth = (width - S(kPadding) * 2) / static_cast<float>(std::size(entries));
    const bool problems = std::any_of(menu.notices.begin(), menu.notices.end(), [](const Notice& notice) { return notice.level >= LogLevel::Warning; });
    PushSize(14);
    int index = 0;
    for (const auto& [name, tab] : entries)
    {
        const ImVec2 start = origin + ImVec2(S(kPadding) + static_cast<float>(index++) * tabWidth, S(kHeader));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton(name, ImVec2(tabWidth, S(kTabs)), ImGuiButtonFlags_EnableNav))
            menu.tab = tab;
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        const bool active = tab == menu.tab;
        const ImVec2 text = ImGui::CalcTextSize(name);
        const ImVec2 textStart = start + ImVec2((tabWidth - text.x) / 2, (S(kTabs) - text.y) / 2);
        draw->AddText(textStart, active || hovered ? kText : kDim, name);
        if (active)
            draw->AddRectFilled(ImVec2(textStart.x - S(6), start.y + S(kTabs) - S(2)), ImVec2(textStart.x + text.x + S(6), start.y + S(kTabs)), kAccent,
                                S(1));
        if (tab == Tab::Status && problems)
            draw->AddCircleFilled(textStart + ImVec2(text.x + S(6), S(3)), S(3), kWarning);
    }
    ImGui::PopFont();
    draw->AddLine(origin + ImVec2(0, S(kHeader + kTabs)), origin + ImVec2(width, S(kHeader + kTabs)), kBorder);
}

void Footer(App& app, ImVec2 origin, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float top = size.y - S(kFooter);
    draw->AddLine(origin + ImVec2(0, top), origin + ImVec2(size.x, top), kBorder);
    const float buttonY = top + (S(kFooter) - S(34)) / 2;

    // Effects stay off while the compare button is held.
    ImGui::SetCursorScreenPos(origin + ImVec2(S(kPadding), buttonY));
    PushSize(14);
    Button("Compare", ImVec2(S(100), S(34)));
    app.compareButton = ImGui::IsItemActive();
    if (!app.compareButton)
        Tooltip("Hold to see the game without effects");
    ImGui::PopFont();

    // Screenshots leave out the menu.
    ImGui::SameLine(0, S(8));
    const ImVec2 cameraStart = ImGui::GetCursorScreenPos();
    const ImVec2 cameraSize(S(34), S(34));
    if (ImGui::InvisibleButton("camera", cameraSize, ImGuiButtonFlags_EnableNav))
        ImGui::OpenPopup("screenshots");
    const bool cameraHovered = ImGui::IsItemHovered();
    HandOnHover();
    Tooltip("Screenshots");
    draw->AddRectFilled(cameraStart, cameraStart + cameraSize, cameraHovered ? kCardHover : kCard, S(6));
    draw->AddRect(cameraStart, cameraStart + cameraSize, kBorder, S(6));
    CameraIcon(draw, cameraStart + cameraSize * 0.5f, S(15), cameraHovered ? kText : kDim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("screenshots"))
    {
        if (ImGui::MenuItem("Screenshot"))
            app.RequestScreenshot(false);
        if (ImGui::MenuItem("Before and after"))
            app.RequestScreenshot(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Open screenshots folder"))
        {
            const fs::path folder = ScreenshotDirectory();
            std::error_code ignored;
            fs::create_directories(folder, ignored);
            platform::Open(folder.string());
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    // The menu shortcut, which also leaves.
    const char* label = "Back to the game";
    const std::string key = app.HotkeyText(kEditModeHotkey);
    PushSize(13.5f);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    PushSize(13);
    const float keyWidth = std::max(ImGui::CalcTextSize(key.c_str()).x + S(14), S(28));
    const float keyHeight = ImGui::GetFontSize() + S(6);
    ImGui::PopFont();
    const ImVec2 backSize(keyWidth + S(8) + labelSize.x, S(34));
    const ImVec2 backStart = origin + ImVec2(size.x - S(kPadding) - backSize.x, buttonY);
    ImGui::SetCursorScreenPos(backStart);
    const bool back = ImGui::InvisibleButton("back", backSize, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImGui::SetCursorScreenPos(backStart + ImVec2(0, (backSize.y - keyHeight) / 2));
    KeyCap(key);
    PushSize(13.5f);
    draw->AddText(backStart + ImVec2(keyWidth + S(8), (backSize.y - labelSize.y) / 2), hovered ? kText : kDim, label);
    ImGui::PopFont();
    if (back)
        app.CloseMenu();
}

void DrawMenu(App& app)
{
    menu.notices = Notices();
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(S(kWidth), io.DisplaySize.x - S(kMargin) * 2);
    const float available = std::max(io.DisplaySize.y - S(kMargin) * 2, 1.0f);
    // A window too short for the menu scrolls all of it, so the text keeps a readable size.
    const ImVec2 size(width, std::max(available, S(menu_layout::MinHeight())));
    const bool scrolls = size.y > available;
    ImGui::SetNextWindowPos(ImVec2(S(kMargin), S(kMargin)));
    ImGui::SetNextWindowSize(ImVec2(width, available));
    ImGui::Begin("Unishade##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     (scrolls ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollWithMouse));
    const ImVec2 origin = ImGui::GetWindowPos() - ImVec2(0, ImGui::GetScrollY());
    Header(app, origin, width);
    Tabs(origin, width);

    const float top = S(kHeader + kTabs) + 1;
    ImGui::SetCursorScreenPos(origin + ImVec2(0, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(kPadding), S(16)));
    ImGui::BeginChild("content", ImVec2(width, size.y - top - S(kFooter)), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar();
    switch (menu.tab)
    {
    case Tab::Presets: PresetsTab(app); break;
    case Tab::Effects: EffectsTab(app); break;
    case Tab::Settings: SettingsTab(app); break;
    case Tab::Status: StatusTab(app); break;
    }
    ImGui::EndChild();
    Footer(app, origin, size);
    // Dialogs show over any tab.
    NameDialog(app);
    UnsavedDialog(app);
    ConfirmDialog(app);
    if (scrolls)
    {
        // Lets the window scroll down to the footer.
        ImGui::SetCursorScreenPos(origin + ImVec2(0, size.y));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::End();
}

void DrawMenuFrame(App& app)
{
    // Escape leaves the menu, unless it closes a popup, ends typing or stops waiting for a shortcut.
    const bool leave = menu.recording < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
                       !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    RecordShortcut(app);

    DrawMenu(app);

    if (menu.recording >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        StopRecording(app);
    if (leave)
        app.CloseMenu();
}

// A short message at the bottom of the game, with a key before it when it has one.
void DrawToast(App& app)
{
    const double left = app.toastUntil - glfwGetTime();
    if (app.toast.empty() || left <= 0)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y - S(48)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(static_cast<float>(left / 0.6), 0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    if (!app.toastKey.empty())
    {
        KeyCap(app.toastKey, 14);
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
    }
    Text(app.toast, kText, 14.5f, -1.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// The interface's scale for the window's monitor. On X11 it is the same on every monitor.
float WindowScale([[maybe_unused]] GLFWwindow* window)
{
#ifdef __APPLE__
    // macOS reports the Retina factor as the content scale, which Dear ImGui applies through the framebuffer scale.
    return 1.0f;
#else
    float scaleX = 1, scaleY = 1;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    return std::max(1.0f, scaleX);
#endif
}
} // namespace

bool InitUi(UiWindow& ui, std::string& error)
{
    ui.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Tab and the arrows move through the controls, and Escape closes popups, as in the menu on Windows.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ui.scale = WindowScale(ui.window);
    // A monitor with another scale, or a new scale in the system's settings. BeginUi applies it.
    glfwSetWindowUserPointer(ui.window, &ui);
    glfwSetWindowContentScaleCallback(ui.window, [](GLFWwindow* window, float, float) {
        static_cast<UiWindow*>(glfwGetWindowUserPointer(window))->rescale = true;
    });

    const std::string font = platform::UiFont(false);
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str()))
        io.Fonts->AddFontDefault();
    // Headings fall back to the regular font.
    const std::string bold = platform::UiFont(true);
    contexts[ui.context].bold = bold.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(bold.c_str());

    if (!ImGui_ImplGlfw_InitForVulkan(ui.window, true))
    {
        error = "Could not start the window's input.";
        return false;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = gpu.apiVersion;
    info.Instance = gpu.instance;
    info.PhysicalDevice = gpu.physicalDevice;
    info.Device = gpu.device;
    info.QueueFamily = gpu.queueFamily;
    info.Queue = gpu.queue;
    info.DescriptorPoolSize = kDescriptorPoolSize;
    info.MinImageCount = ui.surface.minImageCount;
    info.ImageCount = std::max(ui.surface.imageCount, ui.surface.minImageCount);
    info.PipelineInfoMain.RenderPass = ui.surface.renderPass;
    if (!ImGui_ImplVulkan_Init(&info))
    {
        error = "Could not start drawing the interface.";
        return false;
    }
    ui.surface.recreated = false;
    return true;
}

void ShutdownUi(UiWindow& ui)
{
    if (!ui.context)
        return;
    ImGui::SetCurrentContext(ui.context);
    vkDeviceWaitIdle(gpu.device);
    for (auto& [key, picture] : contexts[ui.context].pictures)
        DestroyPicture(picture);
    contexts.erase(ui.context);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(ui.context);
    ui.context = nullptr;
}

void SetWindowIcon([[maybe_unused]] GLFWwindow* window)
{
#ifndef __APPLE__
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(kLogoPng, static_cast<int>(kLogoPngSize), &width, &height, &channels, 4);
    if (!pixels)
        return;
    // A few sizes, for the title bar, the taskbar and the window switcher.
    std::vector<std::vector<uint8_t>> sizes;
    std::vector<GLFWimage> images;
    for (int size : { 32, 64, 256 })
    {
        sizes.push_back(Shrink(pixels, width, height, size));
        images.push_back({ size, size, sizes.back().data() });
    }
    stbi_image_free(pixels);
    glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
#endif
}

void BeginUi(UiWindow& ui)
{
    ImGui::SetCurrentContext(ui.context);
    // Every frame sizes its text and its layout by the scale, so a new one needs nothing else.
    if (std::exchange(ui.rescale, false))
        ui.scale = WindowScale(ui.window);
    if (ui.surface.recreated)
    {
        ImGui_ImplVulkan_SetMinImageCount(ui.surface.minImageCount);
        ui.surface.recreated = false;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EndUi(UiWindow& ui)
{
    ImGui::Render();
    ui.surface.BeginRenderPass();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ui.surface.commands);
    ui.surface.EndFrame();
}

void DrawLauncher(App& app)
{
    scale = app.launcher.scale;
    ApplyStyle(ImGui::GetStyle());
    ImGui::GetStyle().FrameRounding = S(8);
    const ImGuiIO& io = ImGui::GetIO();
    PushSize(kBody);
    // Logos saved since, such as the icon of a game that just started, show now.
    if (glfwGetTime() - launcher.picturesChecked > 2)
    {
        launcher.picturesChecked = glfwGetTime();
        ForgetPictures([](const std::string&) { return true; });
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    // The content scrolls under the strip when the window cannot be as tall as it, and the footer stays at the
    // bottom of the window when the content is shorter.
    const float width = io.DisplaySize.x;
    const float strip = S(3);
    const float footer = LauncherFooterHeight();
    ImGui::SetCursorScreenPos(ImVec2(0, strip));
    ImGui::BeginChild("content", ImVec2(width, std::max(io.DisplaySize.y - strip - footer, 1.0f)));
    const Page page{ ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), S(28), width - S(28) };
    float y = S(24);
    LauncherHeader(page, y);
    PermissionCard(page, y);
    StatusCard(app, page, y);
    EffectsSetupCard(app, page, y);
    GamesSection(app, page, y);
    MessagesSection(page, y);
    ImGui::SetCursorScreenPos(page.At(0, y));
    ImGui::Dummy(ImVec2(width, 0));
    ImGui::EndChild();

    LauncherFooter(app, io.DisplaySize.y - footer, width);
    Rainbow(ImGui::GetWindowDrawList(), ImVec2(0, 0), ImVec2(width, strip));
    PickerDialog(app);
    ImGui::End();
    ImGui::PopFont();
    // The cards that Dear ImGui lays out have their height from the second frame on.
    if (ImGui::GetFrameCount() > 2)
        FitLauncher(app, strip + y + footer);
}

void ResetMenu(App& app)
{
    StopRecording(app);
    app.compareButton = false;
    menu.active.clear();
    // A switch still waiting for an answer about unsaved changes is dropped.
    menu.pendingPreset.clear();
    menu.openUnsavedPopup = false;
}

void DrawOverlay(App& app)
{
    app.menuActiveUniform = nullptr;
    app.menuHoveredUniform = nullptr;
    // The start hint, the only message with a key, has done its job once the menu opens.
    if (app.menuOpen && !app.toastKey.empty())
        app.toastUntil = 0;
    // The menu follows the size of the game's window, as on Windows, counted in the monitor's own units.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    scale = menu_layout::Scale(display.x / app.overlay.scale, display.y / app.overlay.scale) * app.overlay.scale;
    if (scale <= 0)
        return;
    ApplyStyle(ImGui::GetStyle());
    PushSize(14.5f);
    if (app.menuOpen)
        DrawMenuFrame(app);
    DrawToast(app);
    ImGui::PopFont();
}
