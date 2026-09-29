#pragma once

#include <windows.h>

// Draws the host's menu over Roblox through ReShade's ImGui, after the effects, so they do not apply to it.
// Does nothing without the add-on.
void InitMenu();

// Shows which key opens the menu over Roblox for a few seconds, the first time capture starts after the host
// launches.
void ShowStartHint();

// Shortcut actions from the overlay window. They happen on the next frame the overlay shows.
void RequestScreenshot(bool beforeAfter);
void RequestPresetStep(int step);

// Ends what only lasts while the menu is open, such as waiting for a shortcut or comparing with Roblox's
// own picture. Called when input goes back to Roblox.
void ResetMenu();

// The cursor the menu wants, such as a hand over a button.
LPCWSTR MenuCursor();
