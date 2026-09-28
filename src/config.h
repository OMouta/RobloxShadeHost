#pragma once

#include "hotkey.h"

#include <string>

struct InputHotkeys
{
    Hotkey input;
    Hotkey overlay;
};

inline constexpr wchar_t kDefaultToggleKey[] = L"Home";
inline constexpr wchar_t kDefaultOverlayToggleKey[] = L"Ctrl+F8";

// Reads shortcuts from RobloxShadeHost.ini beside the exe into g.hotkeys, creating the file on first run.
// Invalid values are reported and replaced by the defaults.
void LoadInputHotkeys();

// Registers the overlay shortcut and reports a menu shortcut another program already holds.
void RegisterHotkeys();

// Holding a bare key such as Home all the time would break it in every other program, so the menu shortcut is
// only registered while Roblox or the menu is in front. Called every loop.
void UpdateInputHotkey();

// Unregisters both shortcuts until called with false, so the menu can read them as ordinary keys.
void SuspendHotkeys(bool suspended);

// Switches to new shortcuts and saves them. Returns what went wrong, and keeps the current shortcuts, when
// another program holds one of them or the file cannot be written.
std::wstring ChangeHotkeys(const InputHotkeys& hotkeys);

// Whether the menu saves preset changes as they happen, from RobloxShadeHost.ini. On unless turned off.
bool AutoSavePresets();
void SetAutoSavePresets(bool enabled);

// Folder of RobloxShadeHost.exe, with a trailing backslash.
std::wstring ExeDirectory();
