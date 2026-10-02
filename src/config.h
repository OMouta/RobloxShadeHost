#pragma once

#include "hotkey.h"

#include <cstddef>
#include <optional>
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

// Two shortcuts on the same keys, by their index in kShortcuts.
struct ShortcutClash
{
    size_t earlier;
    size_t later;
};

// The first two shortcuts on the same keys, in kShortcuts' order, or nothing when each has keys of its own.
// Shortcuts without a key never clash.
std::optional<ShortcutClash> FindShortcutClash(const InputHotkeys& hotkeys);

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

bool DebugInfoEnabled();
void SetDebugInfoEnabled(bool enabled);

// The user's size for the menu, on top of the size that follows the game's window, from RobloxShadeHost.ini. 1 unless
// changed, and kept between 0.75 and 2.
float MenuScale();
void SetMenuScale(float scale);

// Whether the host asks GitHub for a newer version when it starts, from RobloxShadeHost.ini. On unless turned off.
bool UpdateChecksEnabled();
void SetUpdateChecksEnabled(bool enabled);

// Whether effects stay over the game while another window is in front, from RobloxShadeHost.ini. Off unless turned on.
bool KeepEffectsVisible();
void SetKeepEffectsVisible(bool enabled);

// The most frames a second the overlay shows, from RobloxShadeHost.ini. 0 unless changed, which shows every frame the
// game draws. A limit is kept between kSlowestFrameRate and kFastestFrameRate. The menu is held to it too, so the
// slowest still leaves the menu usable.
constexpr int kSlowestFrameRate = 30;
constexpr int kFastestFrameRate = 500;
int FrameRateLimit();
void SetFrameRateLimit(int fps);

// The percentage of the game's resolution that effects run at, from RobloxShadeHost.ini. 100 unless changed, and never
// under 25.
int EffectResolution();
void SetEffectResolution(int percent);

// The longest side of the picture depth is estimated from, from RobloxShadeHost.ini. Depth Anything V2 expects multiples
// of 14 around 518, which it is unless changed, and it is never larger.
constexpr int kLargestDepthSize = 518;
int DepthSize();
void SetDepthSize(int size);

// The preset last used in a saved game, relative to the presets folder, from RobloxShadeHost.ini. Empty when the
// game has none yet.
std::wstring GamePreset(const std::wstring& game);
void SetGamePreset(const std::wstring& game, const std::wstring& preset);
// Forgets the game's preset, such as when the game is removed or renamed.
void RemoveGamePreset(const std::wstring& game);

// Folder of Unishade.exe, with a trailing backslash.
std::wstring ExeDirectory();
