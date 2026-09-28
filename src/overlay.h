#pragma once

// Creates the overlay window that hosts the swapchain over Roblox.
void CreateOverlayWindow();

// Switches the overlay between passing clicks through to Roblox and receiving them itself, for the menu.
void SetEditMode(bool enabled);

// Leaves edit mode and brings Roblox back to the front.
void ReturnToRoblox();

// Keeps the overlay exactly over Roblox while Roblox is the foreground window (or while editing).
void UpdateOverlay();

// Turns capture and the overlay off or on, like the overlay shortcut.
void ToggleOverlay();
