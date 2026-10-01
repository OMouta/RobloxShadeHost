#include "config.h"
#include "log.h"
#include "state.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <iterator>

namespace
{
// Read from the file once, since the menu and the overlay ask for them every frame. -1, or 0 for the scale, until
// read. Atomic because the update check may ask from its own thread.
std::atomic<int> updateChecks = -1;
std::atomic<int> keepEffectsVisible = -1;
std::atomic<float> menuScale = 0.0f;

std::wstring IniPath()
{
    // Keep the existing filename so upgrades retain shortcuts and menu settings.
    return ExeDirectory() + L"RobloxShadeHost.ini";
}

bool CachedFlag(std::atomic<int>& cached, const wchar_t* name, bool fallback)
{
    int value = cached;
    if (value < 0)
    {
        value = GetPrivateProfileIntW(L"Menu", name, fallback, IniPath().c_str()) != 0;
        cached = value;
    }
    return value != 0;
}

// The new value applies until the host closes even when the file cannot be written.
bool SaveFlag(std::atomic<int>& cached, const wchar_t* name, bool enabled)
{
    cached = enabled;
    return WritePrivateProfileStringW(L"Menu", name, enabled ? L"1" : L"0", IniPath().c_str()) != FALSE;
}

// Also turns a scale that is not a number into the default.
float ValidScale(float scale)
{
    return scale > 0 ? std::clamp(scale, 0.75f, 2.0f) : 1.0f;
}

// A missing entry uses the default. An empty one leaves the shortcut unassigned when allowEmpty is set.
Hotkey ReadHotkey(const std::wstring& path, const wchar_t* name, const wchar_t* fallback, bool allowEmpty)
{
    wchar_t value[128]{};
    const DWORD count = GetPrivateProfileStringW(L"Input", name, fallback, value, static_cast<DWORD>(std::size(value)), path.c_str());
    Hotkey hotkey;
    if (allowEmpty && count == 0)
        return hotkey;
    if (count < std::size(value) - 1 && ParseHotkey(value, hotkey))
        return hotkey;
    Log(LogLevel::Warning, L"%ls=%ls in RobloxShadeHost.ini is not a supported shortcut. Using %ls instead.", name, value, fallback);
    ParseHotkey(fallback, hotkey);
    return hotkey;
}

bool Same(const Hotkey& a, const Hotkey& b)
{
    return a.key == b.key && a.modifiers == b.modifiers;
}

void UseHotkeys(const InputHotkeys& hotkeys)
{
    g.hotkeys = hotkeys;
    g.inputHotkey = FormatHotkey(hotkeys.input);
    g.overlayHotkey = FormatHotkey(hotkeys.overlay);
}

bool Register(const Shortcut& shortcut, const InputHotkeys& hotkeys)
{
    const Hotkey& hotkey = hotkeys.*shortcut.member;
    return !hotkey.key || RegisterHotKey(g.overlay, shortcut.id, hotkey.modifiers, hotkey.key);
}

// Returns the first shortcut that another program holds, or nullptr.
const Shortcut* RegisterSet(bool always, const InputHotkeys& hotkeys)
{
    const Shortcut* taken = nullptr;
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.always == always && !Register(shortcut, hotkeys) && !taken)
            taken = &shortcut;
    return taken;
}

void UnregisterSet(bool always)
{
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.always == always)
            UnregisterHotKey(g.overlay, shortcut.id);
}

void UnregisterAll()
{
    UnregisterSet(true);
    UnregisterSet(false);
    g.gameHotkeysRegistered = false;
}
} // namespace

std::wstring ExeDirectory()
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    const std::wstring path(executable, length);
    return path.substr(0, path.find_last_of(L"\\/") + 1);
}

void LoadInputHotkeys()
{
    const std::wstring path = IniPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        bool written = true;
        for (const Shortcut& shortcut : kShortcuts)
            written &= WritePrivateProfileStringW(L"Input", shortcut.name, shortcut.fallback, path.c_str()) != FALSE;
        if (!written)
            Log(LogLevel::Warning, L"Could not create %ls. Using the default shortcuts.", path.c_str());
    }

    InputHotkeys hotkeys;
    for (const Shortcut& shortcut : kShortcuts)
        hotkeys.*shortcut.member = ReadHotkey(path, shortcut.name, shortcut.fallback, shortcut.member != &InputHotkeys::input);
    // A key can only do one thing, so a later shortcut on the same key is turned off.
    for (size_t i = 1; i < std::size(kShortcuts); ++i)
        for (size_t j = 0; j < i; ++j)
        {
            Hotkey& later = hotkeys.*kShortcuts[i].member;
            if (later.key && Same(later, hotkeys.*kShortcuts[j].member))
            {
                Log(LogLevel::Warning, L"%ls and %ls in RobloxShadeHost.ini are both %ls, so %ls is off. Pick another one in the menu's Settings.",
                    kShortcuts[j].name, kShortcuts[i].name, FormatHotkey(later).c_str(), kShortcuts[i].name);
                later = {};
            }
        }
    UseHotkeys(hotkeys);
}

void RegisterHotkeys()
{
    // Reports taken shortcuts now rather than on the first press in the game.
    for (const Shortcut& shortcut : kShortcuts)
    {
        if (Register(shortcut, g.hotkeys))
            continue;
        const std::wstring key = FormatHotkey(g.hotkeys.*shortcut.member);
        if (shortcut.member == &InputHotkeys::input)
            Log(LogLevel::Warning, L"%ls is in use by another program, so it cannot open the menu. Close that program, or change "
                                   L"ToggleKey in RobloxShadeHost.ini and restart Unishade.",
                key.c_str());
        else
            Log(LogLevel::Warning, L"%ls is in use by another program, so %ls is off. Pick another one in the menu's Settings.", key.c_str(),
                shortcut.name);
    }
    UnregisterSet(false);
}

void UpdateInputHotkey()
{
    const bool wanted = g.target && !g.hotkeysSuspended && (g.editMode || GetForegroundWindow() == g.target);
    if (wanted == g.gameHotkeysRegistered)
        return;
    if (wanted)
        RegisterSet(false, g.hotkeys);
    else
        UnregisterSet(false);
    g.gameHotkeysRegistered = wanted;
}

void SuspendHotkeys(bool suspended)
{
    if (suspended == g.hotkeysSuspended)
        return;
    g.hotkeysSuspended = suspended;
    if (suspended)
        UnregisterAll();
    else
    {
        RegisterSet(true, g.hotkeys);
        UpdateInputHotkey();
    }
}

bool AutoSavePresets()
{
    return GetPrivateProfileIntW(L"Menu", L"AutoSavePresets", 1, IniPath().c_str()) != 0;
}

void SetAutoSavePresets(bool enabled)
{
    if (!WritePrivateProfileStringW(L"Menu", L"AutoSavePresets", enabled ? L"1" : L"0", IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save the auto-save setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

bool DebugInfoEnabled()
{
    return GetPrivateProfileIntW(L"Menu", L"ShowDebugInfo", 0, IniPath().c_str()) != 0;
}

void SetDebugInfoEnabled(bool enabled)
{
    if (!WritePrivateProfileStringW(L"Menu", L"ShowDebugInfo", enabled ? L"1" : L"0", IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save the debug info setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

float MenuScale()
{
    float scale = menuScale;
    if (scale == 0)
    {
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"Menu", L"Scale", L"1", value, static_cast<DWORD>(std::size(value)), IniPath().c_str());
        scale = ValidScale(wcstof(value, nullptr));
        menuScale = scale;
    }
    return scale;
}

void SetMenuScale(float scale)
{
    scale = ValidScale(scale);
    // A slider may set it on every frame, and the file only needs writing when it changes.
    if (scale == MenuScale())
        return;
    menuScale = scale;
    wchar_t value[32]{};
    swprintf_s(value, L"%.2f", scale);
    if (!WritePrivateProfileStringW(L"Menu", L"Scale", value, IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save the menu size to RobloxShadeHost.ini. It applies until Unishade closes.");
}

bool UpdateChecksEnabled()
{
    return CachedFlag(updateChecks, L"CheckForUpdates", true);
}

void SetUpdateChecksEnabled(bool enabled)
{
    if (!SaveFlag(updateChecks, L"CheckForUpdates", enabled))
        Log(LogLevel::Warning, L"Could not save the update check setting to RobloxShadeHost.ini.");
}

bool KeepEffectsVisible()
{
    return CachedFlag(keepEffectsVisible, L"KeepEffectsVisible", false);
}

void SetKeepEffectsVisible(bool enabled)
{
    if (!SaveFlag(keepEffectsVisible, L"KeepEffectsVisible", enabled))
        Log(LogLevel::Warning, L"Could not save the effects visibility setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

std::wstring GamePreset(const std::wstring& game)
{
    std::wstring value(32768, L'\0');
    value.resize(GetPrivateProfileStringW(L"GamePresets", game.c_str(), L"", value.data(), static_cast<DWORD>(value.size()), IniPath().c_str()));
    return value;
}

void SetGamePreset(const std::wstring& game, const std::wstring& preset)
{
    if (!WritePrivateProfileStringW(L"GamePresets", game.c_str(), preset.c_str(), IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save the preset for %ls to RobloxShadeHost.ini.", game.c_str());
}

void RemoveGamePreset(const std::wstring& game)
{
    WritePrivateProfileStringW(L"GamePresets", game.c_str(), nullptr, IniPath().c_str());
}

std::wstring ChangeHotkeys(const InputHotkeys& hotkeys)
{
    const InputHotkeys previous = g.hotkeys;
    UnregisterAll();

    // Registering each new shortcut once shows whether another program holds it.
    std::wstring error;
    const Shortcut* taken = RegisterSet(true, hotkeys);
    if (!taken)
        taken = RegisterSet(false, hotkeys);
    UnregisterAll();
    if (taken)
        error = FormatHotkey(hotkeys.*taken->member) + L" is in use by another program.";
    else
        for (const Shortcut& shortcut : kShortcuts)
            if (!WritePrivateProfileStringW(L"Input", shortcut.name, FormatHotkey(hotkeys.*shortcut.member).c_str(), IniPath().c_str()))
                error = L"Could not save RobloxShadeHost.ini.";

    UseHotkeys(error.empty() ? hotkeys : previous);
    if (error.empty())
        Log(LogLevel::Info, L"Shortcuts changed.");
    RegisterSet(true, g.hotkeys);
    UpdateInputHotkey();
    return error;
}
