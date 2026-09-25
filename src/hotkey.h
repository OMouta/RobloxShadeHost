#pragma once

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
};

inline bool ParseHotkey(std::wstring_view text, Hotkey& result)
{
    Hotkey parsed;
    while (!text.empty())
    {
        const auto separator = text.find(L'+');
        std::wstring token(text.substr(0, separator));
        const auto first = token.find_first_not_of(L" \t");
        if (first == std::wstring::npos)
            return false;
        token = token.substr(first, token.find_last_not_of(L" \t") - first + 1);
        for (auto& character : token)
            character = static_cast<wchar_t>(std::towupper(character));

        UINT modifier = 0;
        if (token == L"CTRL") modifier = MOD_CONTROL;
        else if (token == L"ALT") modifier = MOD_ALT;
        else if (token == L"SHIFT") modifier = MOD_SHIFT;
        else if (token == L"WIN") modifier = MOD_WIN;

        if (modifier)
        {
            if (parsed.key || (parsed.modifiers & modifier))
                return false;
            parsed.modifiers |= modifier;
        }
        else
        {
            if (parsed.key)
                return false;
            if (token.size() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') || (token[0] >= L'0' && token[0] <= L'9')))
                parsed.key = token[0];
            else if (token[0] == L'F' && token.size() >= 2 && token.size() <= 3)
            {
                unsigned number = 0;
                for (size_t i = 1; i < token.size(); ++i)
                {
                    if (token[i] < L'0' || token[i] > L'9')
                        return false;
                    number = number * 10 + token[i] - L'0';
                }
                // Windows reserves F12 for the debugger.
                if (number < 1 || number > 24 || number == 12)
                    return false;
                parsed.key = VK_F1 + number - 1;
            }
            else
            {
                for (const auto& named : kNamedKeys)
                    if (_wcsicmp(token.c_str(), named.name) == 0)
                        parsed.key = named.key;
                if (!parsed.key)
                    return false;
            }
        }
        if (separator == std::wstring_view::npos)
            break;
        text.remove_prefix(separator + 1);
        if (text.empty())
            return false;
    }
    if (!parsed.key)
        return false;
    result = parsed;
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
