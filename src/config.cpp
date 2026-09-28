#include "config.h"
#include "log.h"
#include "state.h"

#include <iterator>

namespace
{
std::wstring IniPath()
{
    return ExeDirectory() + L"RobloxShadeHost.ini";
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

void UnregisterHotkeys()
{
    if (g.inputHotkeyRegistered)
        UnregisterHotKey(g.overlay, kEditModeHotkey);
    g.inputHotkeyRegistered = false;
    UnregisterHotKey(g.overlay, kOverlayToggleHotkey);
}

bool RegisterOverlayHotkey()
{
    return !g.hotkeys.overlay.key || RegisterHotKey(g.overlay, kOverlayToggleHotkey, g.hotkeys.overlay.modifiers, g.hotkeys.overlay.key);
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
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !(WritePrivateProfileStringW(L"Input", L"ToggleKey", kDefaultToggleKey, path.c_str()) &&
          WritePrivateProfileStringW(L"Input", L"OverlayToggleKey", kDefaultOverlayToggleKey, path.c_str())))
        Log(LogLevel::Warning, L"Could not create %ls. Using the default shortcuts.", path.c_str());

    InputHotkeys hotkeys{ ReadHotkey(path, L"ToggleKey", kDefaultToggleKey, false),
                          ReadHotkey(path, L"OverlayToggleKey", kDefaultOverlayToggleKey, true) };
    if (Same(hotkeys.overlay, hotkeys.input))
    {
        Log(LogLevel::Warning, L"The menu and overlay shortcuts are both %ls, so the overlay shortcut is off. Pick another one in the menu's Settings.",
            FormatHotkey(hotkeys.input).c_str());
        hotkeys.overlay = {};
    }
    UseHotkeys(hotkeys);
}

void RegisterHotkeys()
{
    // Reports a taken shortcut now rather than on the first press in Roblox.
    if (RegisterHotKey(g.overlay, kEditModeHotkey, g.hotkeys.input.modifiers, g.hotkeys.input.key))
        UnregisterHotKey(g.overlay, kEditModeHotkey);
    else
        Log(LogLevel::Warning, L"%ls is in use by another program, so it cannot open the menu. Close that program, or change "
                               L"ToggleKey in RobloxShadeHost.ini and restart RobloxShadeHost.",
            g.inputHotkey.c_str());
    if (!RegisterOverlayHotkey())
        Log(LogLevel::Warning, L"%ls is in use by another program, so the overlay shortcut is off. Pick another one in the menu's Settings.",
            g.overlayHotkey.c_str());
}

void UpdateInputHotkey()
{
    const bool wanted = g.target && !g.hotkeysSuspended && (g.editMode || GetForegroundWindow() == g.target);
    if (wanted == g.inputHotkeyRegistered)
        return;
    if (wanted)
    {
        // Retried while wanted, in case the other program lets go of the key.
        g.inputHotkeyRegistered = RegisterHotKey(g.overlay, kEditModeHotkey, g.hotkeys.input.modifiers, g.hotkeys.input.key);
    }
    else
    {
        UnregisterHotKey(g.overlay, kEditModeHotkey);
        g.inputHotkeyRegistered = false;
    }
}

void SuspendHotkeys(bool suspended)
{
    if (suspended == g.hotkeysSuspended)
        return;
    g.hotkeysSuspended = suspended;
    if (suspended)
        UnregisterHotkeys();
    else
    {
        RegisterOverlayHotkey();
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
        Log(LogLevel::Warning, L"Could not save the auto-save setting to RobloxShadeHost.ini. It applies until RobloxShadeHost closes.");
}

std::wstring ChangeHotkeys(const InputHotkeys& hotkeys)
{
    const InputHotkeys previous = g.hotkeys;
    UnregisterHotkeys();

    std::wstring error;
    if (!RegisterHotKey(g.overlay, kEditModeHotkey, hotkeys.input.modifiers, hotkeys.input.key))
        error = FormatHotkey(hotkeys.input) + L" is in use by another program.";
    else
    {
        UnregisterHotKey(g.overlay, kEditModeHotkey);
        if (hotkeys.overlay.key && !RegisterHotKey(g.overlay, kOverlayToggleHotkey, hotkeys.overlay.modifiers, hotkeys.overlay.key))
            error = FormatHotkey(hotkeys.overlay) + L" is in use by another program.";
        else if (!WritePrivateProfileStringW(L"Input", L"ToggleKey", FormatHotkey(hotkeys.input).c_str(), IniPath().c_str()) ||
                 !WritePrivateProfileStringW(L"Input", L"OverlayToggleKey", FormatHotkey(hotkeys.overlay).c_str(), IniPath().c_str()))
        {
            error = L"Could not save RobloxShadeHost.ini.";
            UnregisterHotKey(g.overlay, kOverlayToggleHotkey);
        }
    }

    UseHotkeys(error.empty() ? hotkeys : previous);
    if (error.empty())
        Log(LogLevel::Info, L"Shortcuts changed: menu %ls, overlay %ls", g.inputHotkey.c_str(), g.overlayHotkey.empty() ? L"none" : g.overlayHotkey.c_str());
    else
        RegisterOverlayHotkey();
    UpdateInputHotkey();
    return error;
}
