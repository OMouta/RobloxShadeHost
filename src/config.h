#pragma once

#include "hotkey.h"

#include <string>

struct InputHotkeys
{
    Hotkey input; // opens and closes the menu
    Hotkey overlay;
    Hotkey compare;
    Hotkey screenshot;
    Hotkey beforeAfter;
    Hotkey nextPreset;
    Hotkey previousPreset;
};

// What WM_HOTKEY reports for each shortcut.
enum HotkeyId
{
    kEditModeHotkey = 1,
    kOverlayToggleHotkey,
    kCompareHotkey,
    kScreenshotHotkey,
    kBeforeAfterHotkey,
    kNextPresetHotkey,
    kPreviousPresetHotkey,
};

struct Shortcut
{
    Hotkey InputHotkeys::*member;
    int id;
    const wchar_t* name; // the entry in RobloxShadeHost.ini
    const wchar_t* fallback;
    // Held for as long as the host runs. The others only while the game or the menu is in front, so other
    // programs keep the keys.
    bool always;
};

// Every shortcut, in the order the menu's Settings lists them.
inline constexpr Shortcut kShortcuts[] = {
    { &InputHotkeys::input, kEditModeHotkey, L"ToggleKey", L"Home", false },
    { &InputHotkeys::overlay, kOverlayToggleHotkey, L"OverlayToggleKey", L"Ctrl+F8", true },
    { &InputHotkeys::compare, kCompareHotkey, L"CompareKey", L"F7", false },
    { &InputHotkeys::screenshot, kScreenshotHotkey, L"ScreenshotKey", L"Ctrl+F9", false },
    { &InputHotkeys::beforeAfter, kBeforeAfterHotkey, L"BeforeAfterKey", L"Ctrl+F10", false },
    { &InputHotkeys::nextPreset, kNextPresetHotkey, L"NextPresetKey", L"Ctrl+PageDown", false },
    { &InputHotkeys::previousPreset, kPreviousPresetHotkey, L"PreviousPresetKey", L"Ctrl+PageUp", false },
};

// Reads shortcuts from RobloxShadeHost.ini beside the exe into g.hotkeys, creating the file on first run.
// Invalid values are reported and replaced by the defaults.
void LoadInputHotkeys();

// Registers the shortcuts held all the time and reports shortcuts another program already holds.
void RegisterHotkeys();

// Holding a bare key such as Home all the time would break it in every other program, so most shortcuts are
// only registered while the game or the menu is in front. Called every loop.
void UpdateInputHotkey();

// Unregisters every shortcut until called with false, so the menu can read them as ordinary keys.
void SuspendHotkeys(bool suspended);

// Switches to new shortcuts and saves them. Returns what went wrong, and keeps the current shortcuts, when
// another program holds one of them or the file cannot be written.
std::wstring ChangeHotkeys(const InputHotkeys& hotkeys);

// Whether the menu saves preset changes as they happen, from RobloxShadeHost.ini. On unless turned off.
bool AutoSavePresets();
void SetAutoSavePresets(bool enabled);

// Folder of Unishade.exe, with a trailing backslash.
std::wstring ExeDirectory();
