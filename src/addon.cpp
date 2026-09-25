#include "addon.h"
#include "state.h"

#include <reshade.hpp>

// Shown in ReShade's add-on list.
extern "C" __declspec(dllexport) const char* NAME = "RobloxShadeHost";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Opens the ReShade menu with the host's shortcut and supplies estimated depth when depth estimation is installed.";

namespace
{
bool registered = false;
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
    // Closing the menu with ReShade's own key also returns input to Roblox. This runs inside ReShade's
    // present, so the window changes wait for the message loop.
    if (!open && g.editMode)
        PostMessageW(g.overlay, kMenuClosedMessage, 0, 0);
    return false;
}
} // namespace

bool InitAddon()
{
    if (!reshade::register_addon(GetModuleHandleW(nullptr)))
        return false;
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

void OpenReShadeMenu(bool open)
{
    if (runtime)
        runtime->open_overlay(open, reshade::api::input_source::keyboard);
}

void ShutdownAddon()
{
    if (registered)
        reshade::unregister_addon(GetModuleHandleW(nullptr));
}
