#include "addon.h"
#include "reshade_imgui.h"
#include "state.h"

// Shown in ReShade's add-on list.
extern "C" __declspec(dllexport) const char* NAME = "Unishade";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Draws the Unishade menu and supplies estimated depth when depth estimation is installed.";

namespace
{
bool registered = false;
bool tooOld = false;
bool reshadeMenuOpen = false;
bool allowReShadeMenu = false;
// The host has one swapchain, so ReShade creates one runtime for it.
reshade::api::effect_runtime* runtime = nullptr;

void OnInitRuntime(reshade::api::effect_runtime* created)
{
    runtime = created;
}

void OnDestroyRuntime(reshade::api::effect_runtime* destroyed)
{
    if (runtime == destroyed)
        runtime = nullptr;
}

bool OnOpenOverlay(reshade::api::effect_runtime*, bool open, reshade::api::input_source)
{
    // The host's menu replaces ReShade's, which only opens from the host's menu.
    if (open && !allowReShadeMenu)
        return true;
    reshadeMenuOpen = open;
    return false;
}
} // namespace

bool ReShadeLoaded()
{
    return reshade::internal::get_reshade_module_handle() != nullptr;
}

bool InitAddon()
{
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!reshade::register_addon(module))
    {
        // register_addon also fails when ReShade does not export the ImGui version the menu is built with, but
        // leaves the add-on registered then. Unregistering treats that like a ReShade without add-on support.
        reshade::unregister_addon(module);
        using GetTable = const void* (*)(uint32_t);
        const HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
        const auto getTable = reshadeModule ? reinterpret_cast<GetTable>(GetProcAddress(reshadeModule, "ReShadeGetImGuiFunctionTable")) : nullptr;
        tooOld = reshadeModule && (!getTable || !getTable(IMGUI_VERSION_NUM));
        return false;
    }
    registered = true;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
    return true;
}

bool AddonRegistered()
{
    return registered;
}

bool ReShadeTooOld()
{
    return tooOld;
}

void OpenReShadeMenu(bool open)
{
    if (!runtime)
        return;
    allowReShadeMenu = open;
    runtime->open_overlay(open, reshade::api::input_source::keyboard);
    allowReShadeMenu = false;
}

bool ReShadeMenuOpen()
{
    return reshadeMenuOpen;
}

void ShutdownAddon()
{
    if (registered)
        reshade::unregister_addon(GetModuleHandleW(nullptr));
}
