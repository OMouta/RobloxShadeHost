#pragma once

#include <windows.h>

// Draws the host's menu over the game through ReShade's ImGui, after the effects, so they do not apply to it.
// Does nothing without the add-on.
void InitMenu();

// Shows which key opens the menu over the game for a few seconds, the first time capture starts after the host
// launches.
void ShowStartHint();

// Shortcut actions from the overlay window. They happen on the next frame the overlay shows.
void RequestScreenshot(bool beforeAfter);
void RequestPresetStep(int step);

// Shows the game without effects until key is let go. UpdateHeldCompare notices that, every loop.
void StartHeldCompare(UINT key);
void UpdateHeldCompare();

// Ends what only lasts while the menu is open, such as waiting for a shortcut or comparing with the game's
// own picture. Called when input goes back to the game.
void ResetMenu();

// The cursor the menu wants, such as a hand over a button.
LPCWSTR MenuCursor();
