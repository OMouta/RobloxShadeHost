#pragma once

// Registers the host as a ReShade add-on, so its shortcut can open and close the ReShade menu. Returns false
// when ReShade is missing or refuses add-ons, for example when the add-on is disabled in ReShade.
bool InitAddon();

bool AddonRegistered();

// Opens or closes the ReShade menu. Does nothing without the add-on or before the first frame is shown.
void OpenReShadeMenu(bool open);

void ShutdownAddon();
