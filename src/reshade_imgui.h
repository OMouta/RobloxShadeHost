#pragma once

// Every file includes ReShade through here. reshade.hpp only declares ReShade's ImGui functions when imgui.h
// comes first, and its inline functions, such as register_addon, must be the same in every file.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
