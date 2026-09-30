#include "ui.h"
#include "log.h"
#include "theme.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>

namespace
{
constexpr ImU32 Color(unsigned rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

ImVec4 ColorVec(unsigned rgb, float alpha = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

void ApplyStyle(ImGuiStyle& style, float scale)
{
    using namespace theme;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ColorVec(kText);
    c[ImGuiCol_TextDisabled] = ColorVec(kDim);
    c[ImGuiCol_WindowBg] = ColorVec(kBackground, 0.97f);
    c[ImGuiCol_ChildBg] = ColorVec(0, 0);
    c[ImGuiCol_PopupBg] = ColorVec(0x16171D, 0.98f);
    c[ImGuiCol_Border] = ColorVec(kBorder);
    c[ImGuiCol_BorderShadow] = ColorVec(0, 0);
    c[ImGuiCol_FrameBg] = ColorVec(kCard);
    c[ImGuiCol_FrameBgHovered] = ColorVec(kCardHover);
    c[ImGuiCol_FrameBgActive] = ColorVec(kCardHover);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_MenuBarBg] = ColorVec(kSidebar);
    c[ImGuiCol_ScrollbarBg] = ColorVec(0, 0);
    c[ImGuiCol_ScrollbarGrab] = ColorVec(kBorder);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = ColorVec(kBorderStrong);
    c[ImGuiCol_CheckMark] = ColorVec(kAccentHover);
    c[ImGuiCol_SliderGrab] = ColorVec(kAccent);
    c[ImGuiCol_SliderGrabActive] = ColorVec(kAccentHover);
    c[ImGuiCol_Button] = ColorVec(kCard);
    c[ImGuiCol_ButtonHovered] = ColorVec(kCardHover);
    c[ImGuiCol_ButtonActive] = ColorVec(kBorder);
    c[ImGuiCol_Header] = ColorVec(kAccent, 0.35f);
    c[ImGuiCol_HeaderHovered] = ColorVec(kAccent, 0.5f);
    c[ImGuiCol_HeaderActive] = ColorVec(kAccent, 0.65f);
    c[ImGuiCol_Separator] = ColorVec(kBorder);
    c[ImGuiCol_SeparatorHovered] = ColorVec(kBorderStrong);
    c[ImGuiCol_SeparatorActive] = ColorVec(kAccent);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = ColorVec(kSidebar);
    c[ImGuiCol_TabHovered] = ColorVec(kCardHover);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = ColorVec(kCard);
    c[ImGuiCol_TabSelectedOverline] = ColorVec(kAccent);
    c[ImGuiCol_TextLink] = ColorVec(kAccentHover);
    c[ImGuiCol_TextSelectedBg] = ColorVec(kAccent, 0.35f);
    c[ImGuiCol_NavCursor] = ColorVec(kAccent);
    c[ImGuiCol_ModalWindowDimBg] = ColorVec(0, 0.6f);
    c[ImGuiCol_PlotHistogram] = ColorVec(kAccent);
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = style.GrabRounding = style.TabRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding = ImVec2(16, 14);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.WindowBorderSize = 1.0f;
    style.FontSizeBase = 15.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// The ring in the logo.
void Rainbow(ImDrawList* draw, ImVec2 center, float radius, float thickness)
{
    constexpr float kPi = 3.14159265f;
    const int count = static_cast<int>(std::size(theme::kRainbow));
    for (int i = 0; i < count; ++i)
    {
        const float a0 = -kPi / 2 + 2 * kPi * i / count, a1 = -kPi / 2 + 2 * kPi * (i + 1) / count;
        draw->PathArcTo(center, radius, a0, a1 + 0.02f, 12);
        draw->PathStroke(Color(theme::kRainbow[i]), 0, thickness);
    }
}

void Heading(const char* text)
{
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void Dim(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Color(theme::kDim));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Color(theme::kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(theme::kAccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(theme::kAccentActive));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    return clicked;
}

void Tooltip(const std::string& text)
{
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void NoticeList(float height)
{
    const std::vector<Notice> notices = Notices();
    if (notices.empty())
    {
        Dim("Nothing needs attention.");
        return;
    }
    ImGui::BeginChild("notices", ImVec2(0, height), ImGuiChildFlags_None);
    for (auto it = notices.rbegin(); it != notices.rend(); ++it)
    {
        const unsigned color = it->level == LogLevel::Error ? theme::kError : it->level == LogLevel::Warning ? theme::kWarning
                             : it->level == LogLevel::Ok      ? theme::kSuccess
                                                              : theme::kDim;
        ImGui::PushStyleColor(ImGuiCol_Text, Color(color));
        ImGui::Bullet();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", it->text.c_str());
    }
    ImGui::EndChild();
}

void Card(const char* id, const std::function<void()>& contents)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(theme::kCard));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    contents();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EffectsSetupCard(App& app)
{
    const EffectSetup::State state = app.setup.Read();
    if (app.EffectsInstalled() && !state.running && !state.finished)
        return;
    Card("setup", [&] {
        if (state.running)
        {
            Heading("Installing effects");
            Dim((state.status + (state.detail.empty() ? "" : "\n" + state.detail)).c_str());
            ImGui::ProgressBar(state.fraction, ImVec2(-FLT_MIN, 0));
            if (ImGui::Button("Cancel"))
                app.setup.Cancel();
            return;
        }
        if (state.finished)
        {
            Heading(state.error.empty() ? "Effects are installed" : "Installing effects failed");
            Dim((state.error.empty() ? state.status : state.error).c_str());
            for (const std::string& note : state.notes)
                Dim(("Skipped " + note).c_str());
            if (!state.error.empty() && PrimaryButton("Try again"))
                app.setup.Start();
            return;
        }
        Heading("Effects are not installed yet");
        Dim("Unishade runs ReShade's effects. Download every package from ReShade's official list and the presets made for "
            "Unishade into the data folder.");
        if (PrimaryButton("Download effects and presets"))
            app.setup.Start();
    });
}

// Launcher

void GamesCard(App& app)
{
    Card("games", [&] {
        Heading("Games");
        Dim("Unishade attaches to these as soon as they open.");
        int remove = -1;
        for (size_t i = 0; i < app.autoGames.size(); ++i)
        {
            AutoGame& game = app.autoGames[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox("##enabled", &game.enabled))
                app.SaveGames();
            ImGui::SameLine();
            ImGui::TextUnformatted(game.name.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, Color(theme::kDim));
            ImGui::TextUnformatted(game.executable.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Remove").x - ImGui::GetStyle().FramePadding.x * 2);
            if (ImGui::SmallButton("Remove"))
                remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0)
        {
            app.autoGames.erase(app.autoGames.begin() + remove);
            app.SaveGames();
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Window");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string preview = app.selected ? app.selected->title : "Find saved games automatically";
        if (ImGui::BeginCombo("##window", preview.c_str()))
        {
            if (ImGui::Selectable("Find saved games automatically", !app.selected))
                app.Select(std::nullopt);
            for (const platform::Window& window : app.Windows())
            {
                ImGui::PushID(static_cast<int>(window.id));
                if (ImGui::Selectable(window.title.c_str(), app.selected && app.selected->id == window.id))
                    app.Select(window);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (app.active)
        {
            const std::string executable = platform::ProcessExecutable(app.active->pid);
            const std::string command = platform::ProcessCommand(app.active->pid);
            const bool saved = std::any_of(app.autoGames.begin(), app.autoGames.end(),
                                           [&](const AutoGame& game) { return game.enabled && MatchesProcess(game, executable, command); });
            if (!saved && ImGui::Button(("Attach to " + app.active->title + " automatically").c_str()))
                app.AddActiveGame();
        }
    });
}

void StatusCard(App& app)
{
    Card("status", [&] {
        std::string title, detail;
        unsigned color = theme::kDim;
        if (!app.captureEnabled)
        {
            title = "Overlay is off";
            detail = "Turn it back on with " + app.HotkeyText(kOverlayToggleHotkey) + ".";
        }
        else if (app.active)
        {
            title = "Running on " + app.active->title;
            detail = "Press " + app.HotkeyText(kEditModeHotkey) + " in the game to open the menu.";
            color = theme::kSuccess;
        }
        else
        {
            title = app.selected ? "Waiting for " + app.selected->title : "Waiting for a game";
            detail = "Open a saved game, or pick its window below.";
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float dot = ImGui::GetFontSize() * 0.3f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        draw->AddCircleFilled(ImVec2(at.x + dot, at.y + ImGui::GetFontSize() * 0.62f), dot, Color(color));
        ImGui::Dummy(ImVec2(dot * 2, 0));
        ImGui::SameLine();
        Heading(title.c_str());
        Dim(detail.c_str());
        if (!app.lastCaptureError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(theme::kWarning));
            ImGui::TextWrapped("%s", app.lastCaptureError.c_str());
            ImGui::PopStyleColor();
        }
        const auto [loaded, total] = app.runtime.LoadingProgress();
        if (app.runtime.Loading())
            ImGui::ProgressBar(total ? float(loaded) / float(total) : 0.0f, ImVec2(-FLT_MIN, 0),
                               ("Compiling effects " + std::to_string(loaded) + " of " + std::to_string(total)).c_str());
        if (ImGui::Button(app.captureEnabled ? "Turn overlay off" : "Turn overlay on"))
            app.ToggleOverlay();
    });
}

void PermissionCard(App&)
{
    if (platform::HasCapturePermission())
        return;
    Card("permission", [&] {
        Heading("Unishade needs to record the screen");
        Dim("macOS asks before a program can copy another program's window. Allow Unishade under Privacy & Security, "
            "Screen & System Audio Recording, then open Unishade again.");
        if (PrimaryButton("Allow screen recording"))
            platform::RequestCapturePermission();
    });
}

// Menu

struct MenuState
{
    int selectedTechnique = -1;
    char search[128] = {};
    char presetName[128] = {};
    bool copyPreset = true;
    std::string presetError;
    fs::path pendingPreset; // waiting for an answer about unsaved changes
    int recording = -1;     // the shortcut being recorded
    InputHotkeys editing;
    std::string shortcutError;
};
MenuState menu;

void DrawParameter(App& app, fx::Effect& effect, const fx::Uniform& uniform)
{
    const reshadefx::type& type = uniform.type;
    const int components = static_cast<int>(type.components());
    if (type.is_array() || type.is_matrix() || components > 4)
        return;
    bool changed = false;
    ImGui::PushID(uniform.name.c_str());
    if (!uniform.text.empty())
        Dim(uniform.text.c_str());
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 2);
    const char* label = uniform.label.c_str();
    const bool bounded = uniform.min > std::numeric_limits<float>::lowest() && uniform.max < std::numeric_limits<float>::max();

    if (type.is_boolean())
    {
        int value = 0;
        app.runtime.GetValue(effect, uniform, &value, 1);
        bool on = value != 0;
        if ((changed = ImGui::Checkbox(label, &on)))
        {
            value = on;
            app.runtime.SetValue(effect, uniform, &value, 1);
        }
    }
    else if (!uniform.items.empty() && !type.is_floating_point() && components == 1)
    {
        int value = 0;
        app.runtime.GetValue(effect, uniform, &value, 1);
        if (uniform.uiType == "radio")
        {
            ImGui::TextUnformatted(label);
            int index = 0;
            for (const char* item = uniform.items.c_str(); *item; item += strlen(item) + 1, ++index)
            {
                ImGui::SameLine();
                changed |= ImGui::RadioButton(item, &value, index);
            }
        }
        else
            changed = ImGui::Combo(label, &value, uniform.items.c_str());
        if (changed)
            app.runtime.SetValue(effect, uniform, &value, 1);
    }
    else if (uniform.uiType == "color" && type.is_floating_point() && components >= 3)
    {
        float value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        changed = components == 3 ? ImGui::ColorEdit3(label, value) : ImGui::ColorEdit4(label, value);
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    else if (type.is_floating_point())
    {
        float value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        if (uniform.uiType == "slider" && bounded)
            changed = ImGui::SliderScalarN(label, ImGuiDataType_Float, value, components, &uniform.min, &uniform.max, "%.3f");
        else if (uniform.uiType == "input")
            changed = ImGui::InputScalarN(label, ImGuiDataType_Float, value, components);
        else
            changed = ImGui::DragScalarN(label, ImGuiDataType_Float, value, components, std::max(uniform.step, 0.0001f),
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
            changed = ImGui::SliderScalarN(label, ImGuiDataType_S32, value, components, &low, &high);
        else if (uniform.uiType == "input")
            changed = ImGui::InputScalarN(label, ImGuiDataType_S32, value, components);
        else
            changed = ImGui::DragScalarN(label, ImGuiDataType_S32, value, components, std::max(uniform.step, 0.2f), bounded ? &low : nullptr,
                                         bounded ? &high : nullptr);
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    Tooltip(uniform.tooltip);
    ImGui::SameLine();
    if (ImGui::SmallButton("R"))
    {
        app.runtime.ResetValue(effect, uniform);
        changed = true;
    }
    Tooltip("Reset to the effect's default");
    if (changed)
        app.runtime.SetDirty();
    ImGui::PopID();
}

void DrawParameters(App& app, fx::Effect& effect)
{
    // Grouped by category in the order they first appear, as in ReShade.
    std::vector<std::string> categories;
    for (const fx::Uniform& uniform : effect.uniforms)
        if (!uniform.hidden && std::find(categories.begin(), categories.end(), uniform.category) == categories.end())
            categories.push_back(uniform.category);
    if (categories.empty())
    {
        Dim("This effect has no settings.");
        return;
    }
    for (const std::string& category : categories)
    {
        if (!category.empty() && !ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        for (const fx::Uniform& uniform : effect.uniforms)
            if (!uniform.hidden && uniform.category == category)
                DrawParameter(app, effect, uniform);
    }
    if (ImGui::Button("Reset all"))
    {
        for (const fx::Uniform& uniform : effect.uniforms)
            if (uniform.source.empty())
                app.runtime.ResetValue(effect, uniform);
        app.runtime.SetDirty();
    }
}

void EffectsTab(App& app)
{
    auto& techniques = app.runtime.Techniques();
    auto& effects = app.runtime.Effects();
    if (app.runtime.Loading())
    {
        const auto [loaded, total] = app.runtime.LoadingProgress();
        ImGui::ProgressBar(total ? float(loaded) / float(total) : 0.0f, ImVec2(-FLT_MIN, 0),
                           ("Compiling effects " + std::to_string(loaded) + " of " + std::to_string(total)).c_str());
    }
    if (techniques.empty() && !app.runtime.Loading())
    {
        Dim("No effects are installed. Install them from the launcher.");
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search effects", menu.search, sizeof(menu.search));
    const std::string search = Lowercase(menu.search);

    const float parametersHeight = menu.selectedTechnique >= 0 ? ImGui::GetContentRegionAvail().y * 0.5f : 0.0f;
    ImGui::BeginChild("techniques", ImVec2(0, parametersHeight > 0 ? ImGui::GetContentRegionAvail().y - parametersHeight : 0));
    int moveFrom = -1, moveTo = -1;
    // Turned-on effects first, in the order they run, then the rest.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
            ImGui::SeparatorText("More effects");
        for (int i = 0; i < static_cast<int>(techniques.size()); ++i)
        {
            fx::Technique& technique = techniques[i];
            if (technique.hidden || technique.enabled != (pass == 0))
                continue;
            const fx::Effect& effect = effects[technique.effect];
            const std::string label = technique.label.empty() ? technique.name : technique.label;
            if (!search.empty() && Lowercase(label).find(search) == std::string::npos && Lowercase(effect.file).find(search) == std::string::npos)
                continue;
            ImGui::PushID(i);
            bool enabled = technique.enabled;
            if (ImGui::Checkbox("##on", &enabled))
                app.runtime.SetEnabled(i, enabled);
            ImGui::SameLine();
            if (ImGui::Selectable((label + "##row").c_str(), menu.selectedTechnique == i))
                menu.selectedTechnique = menu.selectedTechnique == i ? -1 : i;
            Tooltip(technique.tooltip.empty() ? effect.file : technique.tooltip + "\n\n" + effect.file);
            if (technique.enabled)
            {
                if (ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload("technique", &i, sizeof(i));
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("technique"))
                    {
                        moveFrom = *static_cast<const int*>(payload->Data);
                        moveTo = i;
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            if (effect.gpuFailed)
            {
                ImGui::SameLine();
                ImGui::TextColored(ColorVec(theme::kWarning), "(failed)");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (moveFrom >= 0)
    {
        app.runtime.MoveTechnique(moveFrom, moveTo);
        if (menu.selectedTechnique == moveFrom)
            menu.selectedTechnique = moveTo;
    }

    if (menu.selectedTechnique >= 0 && menu.selectedTechnique < static_cast<int>(techniques.size()))
    {
        const fx::Technique& technique = techniques[menu.selectedTechnique];
        fx::Effect& effect = effects[technique.effect];
        ImGui::SeparatorText((technique.label.empty() ? technique.name : technique.label).c_str());
        ImGui::BeginChild("parameters");
        DrawParameters(app, effect);
        ImGui::EndChild();
    }
}

void PresetsTab(App& app)
{
    const fs::path current = app.runtime.PresetPath();
    ImGui::BeginChild("presets", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4));
    for (const fs::path& preset : app.Presets())
    {
        const bool active = preset == current;
        std::string label = preset.stem().string();
        if (active && app.runtime.Dirty())
            label += "  (changed)";
        if (ImGui::Selectable((label + "##" + preset.string()).c_str(), active) && !active)
        {
            if (!app.SwitchPreset(preset, app.settings.autoSavePresets, false))
            {
                menu.pendingPreset = preset;
                ImGui::OpenPopup("Unsaved changes");
            }
        }
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s has changes that are not saved.", current.stem().c_str());
        if (PrimaryButton("Save"))
        {
            app.SwitchPreset(menu.pendingPreset, true, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard"))
        {
            app.SwitchPreset(menu.pendingPreset, false, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputTextWithHint("##name", "New preset name", menu.presetName, sizeof(menu.presetName));
    ImGui::SameLine();
    if (ImGui::Button("Create") && menu.presetName[0])
    {
        if (app.NewPreset(menu.presetName, menu.copyPreset, menu.presetError))
        {
            menu.presetName[0] = 0;
            menu.presetError.clear();
        }
    }
    ImGui::Checkbox("Start from the current preset", &menu.copyPreset);
    if (!menu.presetError.empty())
        ImGui::TextColored(ColorVec(theme::kError), "%s", menu.presetError.c_str());
    ImGui::BeginDisabled(!app.runtime.Dirty());
    if (ImGui::Button("Save"))
        app.runtime.SavePreset();
    ImGui::SameLine();
    if (ImGui::Button("Discard changes"))
        app.SwitchPreset(current, false, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open folder"))
        platform::Open(PresetsDirectory().string());
}

void SettingsTab(App& app)
{
    Heading("Shortcuts");
    if (menu.recording < 0)
        menu.editing = app.settings.hotkeys;
    for (const Shortcut& shortcut : kShortcuts)
    {
        ImGui::PushID(shortcut.id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(shortcut.label);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
        const bool recording = menu.recording == shortcut.id;
        const std::string text = recording ? "Press the new shortcut..." : FormatHotkey(menu.editing.*shortcut.member);
        if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0)) && !recording)
        {
            menu.recording = shortcut.id;
            menu.shortcutError.clear();
            app.SuspendHotkeys(true);
        }
        if (recording)
        {
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
                changed.*shortcut.member = hotkey;
                menu.recording = -1;
                app.SuspendHotkeys(false);
                if (!app.ChangeHotkeys(changed, menu.shortcutError))
                    menu.editing = app.settings.hotkeys;
                break;
            }
        }
        ImGui::PopID();
    }
    if (!menu.shortcutError.empty())
        ImGui::TextColored(ColorVec(theme::kError), "%s", menu.shortcutError.c_str());

    ImGui::Spacing();
    Heading("Presets");
    bool autoSave = app.settings.autoSavePresets;
    if (ImGui::Checkbox("Save preset changes as they happen", &autoSave))
    {
        app.settings.autoSavePresets = autoSave;
        if (autoSave && app.runtime.Dirty())
            app.runtime.SavePreset();
        SaveSettings(app.settings);
    }

    ImGui::Spacing();
    Heading("Effects");
    for (const fs::path& path : app.settings.effectPaths)
        Dim(path.c_str());
    if (ImGui::Button("Reload effects"))
        app.runtime.Reload();
    ImGui::SameLine();
    if (ImGui::Button("Open data folder"))
        platform::Open(DataDirectory().string());
}

void StatusTab(App& app)
{
    Heading("Notices");
    NoticeList(ImGui::GetContentRegionAvail().y * 0.4f);
    ImGui::Spacing();
    size_t failed = 0;
    for (const fx::Effect& effect : app.runtime.Effects())
        failed += !effect.compiled;
    Heading(("Effects that did not compile (" + std::to_string(failed) + ")").c_str());
    ImGui::BeginChild("failed");
    for (const fx::Effect& effect : app.runtime.Effects())
        if (!effect.compiled && ImGui::CollapsingHeader(effect.file.c_str()))
        {
            ImGui::PushTextWrapPos(0);
            Dim(effect.errors.c_str());
            ImGui::PopTextWrapPos();
        }
    ImGui::EndChild();
}

void Menu(App& app, ImVec2 display, float scale)
{
    const float margin = 16 * scale;
    const float width = std::min(470 * scale, display.x - margin * 2);
    ImGui::SetNextWindowPos(ImVec2(margin, margin));
    ImGui::SetNextWindowSize(ImVec2(width, display.y - margin * 2));
    ImGui::Begin("Unishade", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float ring = ImGui::GetFontSize() * 0.55f;
    Rainbow(ImGui::GetWindowDrawList(), ImVec2(at.x + ring, at.y + ring * 1.1f), ring, ring * 0.35f);
    ImGui::Dummy(ImVec2(ring * 2, 0));
    ImGui::SameLine();
    Heading("Unishade");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 4.2f);
    bool effects = app.effectsEnabled;
    if (ImGui::Checkbox("Effects", &effects))
        app.effectsEnabled = effects;
    Tooltip("Turns every effect off or on at once");

    // The footer stays at the bottom of the panel, below whichever tab is open.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("body", ImVec2(0, -footer));
    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem("Effects"))
        {
            EffectsTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Presets"))
        {
            PresetsTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings"))
        {
            SettingsTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Status"))
        {
            StatusTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Button("Hold to compare");
    app.compareButton = ImGui::IsItemActive();
    ImGui::SameLine();
    if (ImGui::Button("Screenshot"))
        app.RequestScreenshot(false);
    if (!app.settings.autoSavePresets)
    {
        ImGui::SameLine();
        ImGui::BeginDisabled(!app.runtime.Dirty());
        if (ImGui::Button("Save preset"))
            app.runtime.SavePreset();
        ImGui::EndDisabled();
    }
    if (PrimaryButton(("Back to game (" + app.HotkeyText(kEditModeHotkey) + ")").c_str(), ImVec2(-FLT_MIN, 0)))
        app.CloseMenu();
    ImGui::End();
}

void Toast(App& app, ImVec2 display, float scale)
{
    if (app.toast.empty() || glfwGetTime() > app.toastUntil)
        return;
    ImGui::SetNextWindowPos(ImVec2(display.x / 2, display.y - 40 * scale), 0, ImVec2(0.5f, 1));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(app.toast.c_str());
    ImGui::End();
}
} // namespace

bool InitUi(UiWindow& ui, std::string& error)
{
    ui.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    float scaleX = 1, scaleY = 1;
    glfwGetWindowContentScale(ui.window, &scaleX, &scaleY);
#ifdef __APPLE__
    // macOS reports the Retina factor here, which Dear ImGui applies through the framebuffer scale instead.
    ui.scale = 1.0f;
#else
    ui.scale = std::max(1.0f, scaleX);
#endif
    ApplyStyle(ImGui::GetStyle(), ui.scale);

    const std::string font = platform::UiFont();
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str()))
        io.Fonts->AddFontDefault();

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
    info.DescriptorPoolSize = 64;
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
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(ui.context);
    ui.context = nullptr;
}

void BeginUi(UiWindow& ui)
{
    ImGui::SetCurrentContext(ui.context);
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
    const ImGuiIO& io = ImGui::GetIO();
    const float scale = app.launcher.scale;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Color(theme::kBackground));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float ring = 14 * scale;
    Rainbow(ImGui::GetWindowDrawList(), ImVec2(at.x + ring, at.y + ring), ring, ring * 0.35f);
    ImGui::Dummy(ImVec2(ring * 2 + 4 * scale, ring * 2));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted("Unishade");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", UNISHADE_VERSION);
    ImGui::EndGroup();
    ImGui::Spacing();

    ImGui::BeginChild("content", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4 * scale));
    PermissionCard(app);
    StatusCard(app);
    EffectsSetupCard(app);
    GamesCard(app);
    Card("notices", [&] {
        Heading("Notices");
        NoticeList(0);
    });
    ImGui::EndChild();

    if (ImGui::TextLink("Data folder"))
        platform::Open(DataDirectory().string());
    ImGui::SameLine();
    if (ImGui::TextLink("Log"))
        platform::Open(LogPath().string());
    ImGui::SameLine();
    if (ImGui::TextLink("Screenshots"))
    {
        fs::create_directories(ScreenshotDirectory());
        platform::Open(ScreenshotDirectory().string());
    }
    ImGui::SameLine();
    if (ImGui::TextLink("Docs"))
        platform::Open("https://unishade.me/docs/");
    ImGui::End();
}

void ResetMenu(App& app)
{
    if (menu.recording >= 0)
        app.SuspendHotkeys(false);
    menu.recording = -1;
    app.compareButton = false;
}

void DrawOverlay(App& app)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (app.menuOpen)
        Menu(app, display, app.overlay.scale);
    Toast(app, display, app.overlay.scale);
}
