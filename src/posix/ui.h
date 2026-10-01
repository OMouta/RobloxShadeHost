#pragma once

#include "app.h"

// Dear ImGui for the launcher and the menu, styled with Setup's colors like on Windows.
bool InitUi(UiWindow& ui, std::string& error);
void ShutdownUi(UiWindow& ui);

// Starts a Dear ImGui frame in the window's context.
void BeginUi(UiWindow& ui);
// Draws the frame into the window's swapchain image, which BeginFrame acquired, and presents it.
void EndUi(UiWindow& ui);

void DrawLauncher(App& app);
// The menu when it is open, and short messages at the bottom of the game when it is not.
void DrawOverlay(App& app);
// Ends what only lasts while the menu is open, such as recording a shortcut.
void ResetMenu(App& app);
