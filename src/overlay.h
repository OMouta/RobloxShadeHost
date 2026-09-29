#pragma once

// Creates the overlay window that hosts the swapchain over the game.
void CreateOverlayWindow();

// Switches the overlay between passing clicks through to the game and receiving them itself, for the menu.
void SetEditMode(bool enabled);

// Leaves edit mode and brings the game back to the front.
void ReturnToGame();

// Keeps the overlay exactly over the game while the game is the foreground window (or while editing).
void UpdateOverlay();

// Turns capture and the overlay off or on, like the overlay shortcut.
void ToggleOverlay();
