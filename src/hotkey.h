#pragma once

#include "hotkey_text.h"

#include <windows.h>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>

struct Hotkey
{
    UINT modifiers = MOD_NOREPEAT;
    UINT key = 0;
};

struct NamedKey
{
    const wchar_t* name;
    UINT key;
};

// Keys other than letters, digits and function keys.
inline constexpr NamedKey kNamedKeys[] = {
    { L"Home", VK_HOME }, { L"End", VK_END }, { L"Insert", VK_INSERT },
    { L"Delete", VK_DELETE }, { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT },
    { L"Pause", VK_PAUSE }, { L"ScrollLock", VK_SCROLL }, { L"Space", VK_SPACE },
    { L"Tab", VK_TAB }, { L"Escape", VK_ESCAPE },
    { L"Numpad0", VK_NUMPAD0 }, { L"Numpad1", VK_NUMPAD1 }, { L"Numpad2", VK_NUMPAD2 },
    { L"Numpad3", VK_NUMPAD3 }, { L"Numpad4", VK_NUMPAD4 }, { L"Numpad5", VK_NUMPAD5 },
    { L"Numpad6", VK_NUMPAD6 }, { L"Numpad7", VK_NUMPAD7 }, { L"Numpad8", VK_NUMPAD8 },
    { L"Numpad9", VK_NUMPAD9 }, { L"NumpadMultiply", VK_MULTIPLY }, { L"NumpadAdd", VK_ADD },
    { L"NumpadSubtract", VK_SUBTRACT }, { L"NumpadDecimal", VK_DECIMAL }, { L"NumpadDivide", VK_DIVIDE },
};

// The rules for writing shortcuts are shared with macOS and Linux. Shortcut names are ASCII.
inline bool ParseHotkey(std::wstring_view text, Hotkey& result)
{
    std::string narrow;
    for (const wchar_t c : text)
    {
        if (c > 127)
            return false;
        narrow += static_cast<char>(c);
    }
    const auto modifierBit = [](std::string_view name) -> unsigned {
        using hotkey_text::SameName;
        return SameName(name, "Ctrl") ? MOD_CONTROL : SameName(name, "Alt") ? MOD_ALT : SameName(name, "Shift") ? MOD_SHIFT
             : SameName(name, "Win")  ? MOD_WIN
                                      : 0;
    };
    unsigned modifiers = 0;
    std::string name;
    if (!hotkey_text::Split(narrow, modifierBit, modifiers, name))
        return false;

    UINT key = 0;
    if (const char c = hotkey_text::Character(name))
        key = static_cast<UINT>(c);
    else if (const int number = hotkey_text::FunctionKey(name))
    {
        // Windows reserves F12 for the debugger.
        if (number == 12)
            return false;
        key = static_cast<UINT>(VK_F1 + number - 1);
    }
    else
        for (const auto& named : kNamedKeys)
            if (_wcsicmp(std::wstring(name.begin(), name.end()).c_str(), named.name) == 0)
                key = named.key;
    if (!key)
        return false;
    result = { MOD_NOREPEAT | modifiers, key };
    return true;
}

// Writes a hotkey the way ParseHotkey reads it, such as Ctrl+Shift+F8. Returns an empty string for a key
// ParseHotkey does not accept.
inline std::wstring FormatHotkey(const Hotkey& hotkey)
{
    std::wstring key;
    if ((hotkey.key >= 'A' && hotkey.key <= 'Z') || (hotkey.key >= '0' && hotkey.key <= '9'))
        key = static_cast<wchar_t>(hotkey.key);
    else if (hotkey.key >= VK_F1 && hotkey.key <= VK_F24 && hotkey.key != VK_F12)
        key = L"F" + std::to_wstring(hotkey.key - VK_F1 + 1);
    for (const auto& named : kNamedKeys)
        if (hotkey.key == named.key)
            key = named.name;
    if (key.empty())
        return {};

    std::wstring text;
    if (hotkey.modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (hotkey.modifiers & MOD_ALT) text += L"Alt+";
    if (hotkey.modifiers & MOD_SHIFT) text += L"Shift+";
    if (hotkey.modifiers & MOD_WIN) text += L"Win+";
    return text + key;
}
