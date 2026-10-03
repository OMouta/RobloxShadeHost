#pragma once

// Whether ReShade was loaded into the host, which happens when it is installed beside the exe.
bool ReShadeLoaded();

// Registers the host as a ReShade add-on, which the menu and depth estimation need. Returns false when ReShade
// is missing, refuses add-ons, for example when the add-on is disabled in ReShade, or is too old for the menu.
bool InitAddon();

bool AddonRegistered();

// Whether the installed ReShade is too old for the menu. Only meaningful when the add-on is not registered.
bool ReShadeTooOld();

// Opens or closes ReShade's own menu. ReShade's own key cannot open it, only this. Does nothing without the
// add-on or before the first frame is shown.
void OpenReShadeMenu(bool open);

bool ReShadeMenuOpen();

// Whether ReShade is making the effects it loaded, which it does one per frame, so frames are worth showing at full
// rate meanwhile. Always false without the add-on.
bool ReShadeLoadingEffects();

// Whether ReShade compiles effects on its own threads. Anything that resets its effects, such as resizing the swapchain,
// waits for the effects it is compiling, which can take minutes. Always false without the add-on.
bool ReShadeCompilingEffects();

void ShutdownAddon();
