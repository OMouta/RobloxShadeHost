#include "hotkeys.h"

#include <algorithm>
#include <cctype>
#include <strings.h>

namespace
{
struct NamedKey
{
    const char* name;
    ImGuiKey key;
};

// Keys other than letters, digits and function keys.
constexpr NamedKey kNamedKeys[] = {
    { "Home", ImGuiKey_Home }, { "End", ImGuiKey_End }, { "Insert", ImGuiKey_Insert },
    { "Delete", ImGuiKey_Delete }, { "PageUp", ImGuiKey_PageUp }, { "PageDown", ImGuiKey_PageDown },
    { "Pause", ImGuiKey_Pause }, { "ScrollLock", ImGuiKey_ScrollLock }, { "Space", ImGuiKey_Space },
    { "Tab", ImGuiKey_Tab }, { "Escape", ImGuiKey_Escape }, { "Left", ImGuiKey_LeftArrow },
    { "Right", ImGuiKey_RightArrow }, { "Up", ImGuiKey_UpArrow }, { "Down", ImGuiKey_DownArrow },
};

std::string Trim(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}
} // namespace

bool IsShortcutKey(ImGuiKey key)
{
    if ((key >= ImGuiKey_A && key <= ImGuiKey_Z) || (key >= ImGuiKey_0 && key <= ImGuiKey_9) || (key >= ImGuiKey_F1 && key <= ImGuiKey_F24))
        return true;
    return std::any_of(std::begin(kNamedKeys), std::end(kNamedKeys), [key](const NamedKey& named) { return named.key == key; });
}

bool ParseHotkey(std::string_view text, Hotkey& result)
{
    Hotkey parsed;
    while (!text.empty())
    {
        const size_t separator = text.find('+');
        const std::string token = Trim(text.substr(0, separator));
        if (token.empty())
            return false;

        unsigned modifier = 0;
        const char* name = token.c_str();
        if (!strcasecmp(name, "Ctrl") || !strcasecmp(name, "Control"))
            modifier = kCtrl;
        else if (!strcasecmp(name, "Alt") || !strcasecmp(name, "Option") || !strcasecmp(name, "Opt"))
            modifier = kAlt;
        else if (!strcasecmp(name, "Shift"))
            modifier = kShift;
        else if (!strcasecmp(name, "Win") || !strcasecmp(name, "Super") || !strcasecmp(name, "Cmd") || !strcasecmp(name, "Command"))
            modifier = kSuper;

        if (modifier)
        {
            if (parsed.key != ImGuiKey_None || (parsed.modifiers & modifier))
                return false;
            parsed.modifiers |= modifier;
        }
        else
        {
            if (parsed.key != ImGuiKey_None)
                return false;
            const char first = static_cast<char>(std::toupper(static_cast<unsigned char>(token[0])));
            if (token.size() == 1 && first >= 'A' && first <= 'Z')
                parsed.key = static_cast<ImGuiKey>(ImGuiKey_A + (first - 'A'));
            else if (token.size() == 1 && first >= '0' && first <= '9')
                parsed.key = static_cast<ImGuiKey>(ImGuiKey_0 + (first - '0'));
            else if (first == 'F' && token.size() >= 2 && token.size() <= 3 &&
                     std::all_of(token.begin() + 1, token.end(), [](char c) { return c >= '0' && c <= '9'; }))
            {
                const int number = std::stoi(token.substr(1));
                if (number < 1 || number > 24)
                    return false;
                parsed.key = static_cast<ImGuiKey>(ImGuiKey_F1 + number - 1);
            }
            else
            {
                for (const NamedKey& named : kNamedKeys)
                    if (!strcasecmp(name, named.name))
                        parsed.key = named.key;
                if (parsed.key == ImGuiKey_None)
                    return false;
            }
        }
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1);
        if (text.empty())
            return false;
    }
    if (parsed.key == ImGuiKey_None)
        return false;
    result = parsed;
    return true;
}

std::string FormatHotkey(const Hotkey& hotkey)
{
    std::string key;
    if (hotkey.key >= ImGuiKey_A && hotkey.key <= ImGuiKey_Z)
        key = static_cast<char>('A' + (hotkey.key - ImGuiKey_A));
    else if (hotkey.key >= ImGuiKey_0 && hotkey.key <= ImGuiKey_9)
        key = static_cast<char>('0' + (hotkey.key - ImGuiKey_0));
    else if (hotkey.key >= ImGuiKey_F1 && hotkey.key <= ImGuiKey_F24)
        key = "F" + std::to_string(hotkey.key - ImGuiKey_F1 + 1);
    for (const NamedKey& named : kNamedKeys)
        if (hotkey.key == named.key)
            key = named.name;
    if (key.empty())
        return {};

    std::string text;
    if (hotkey.modifiers & kCtrl)
        text += "Ctrl+";
#ifdef __APPLE__
    if (hotkey.modifiers & kAlt)
        text += "Option+";
    if (hotkey.modifiers & kShift)
        text += "Shift+";
    if (hotkey.modifiers & kSuper)
        text += "Cmd+";
#else
    if (hotkey.modifiers & kAlt)
        text += "Alt+";
    if (hotkey.modifiers & kShift)
        text += "Shift+";
    if (hotkey.modifiers & kSuper)
        text += "Super+";
#endif
    return text + key;
}
