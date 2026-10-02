#pragma once

// The host's window outside the game. Its Games tab shows whether the host is waiting for a game or running on
// one, the saved games, and what needs attention. Its Settings tab has the menu's settings and whether the host
// starts with Windows. Minimizing it leaves the host running from its notification area icon, and closing it
// quits the host. Drawn with GDI+, since ReShade would draw its effects on any Direct3D window in
// this process.
void CreateLauncher();

// Redraws the launcher when what it shows has changed, unless it is hidden or minimized. Called every loop.
void UpdateLauncher();

// Also removes the notification area icon. Does nothing more when called again.
void DestroyLauncher();

// A second copy of the host finds the running one's launcher by this class and brings it to the front.
// Shared with earlier hosts when a second instance brings the launcher forward.
inline constexpr wchar_t kLauncherClass[] = L"RobloxShadeHostLauncher";
