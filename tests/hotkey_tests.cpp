#include "../src/hotkey.h"
#include <cstdio>

int main()
{
    const struct { const wchar_t* text; UINT modifiers; UINT key; } valid[] = {
        { L"Ctrl+Home", MOD_CONTROL, VK_HOME },
        { L"  cTrL + sHiFt + f8  ", MOD_CONTROL | MOD_SHIFT, VK_F8 },
        { L"F1", 0, VK_F1 }, { L"F24", 0, VK_F24 },
        { L"Alt+0", MOD_ALT, '0' }, { L"Win+Shift+P", MOD_WIN | MOD_SHIFT, 'P' },
        { L"PageDown", 0, VK_NEXT }, { L"ScrollLock", 0, VK_SCROLL },
    };
    for (const auto& test : valid)
    {
        Hotkey result;
        if (!ParseHotkey(test.text, result) || result.modifiers != (test.modifiers | MOD_NOREPEAT) || result.key != test.key)
        {
            std::printf("Failed valid shortcut: %ls\n", test.text);
            return 1;
        }
    }
    const wchar_t* invalid[] = {
        L"", L" ", L"Ctrl", L"Ctrl+", L"+Home", L"Ctrl++Home", L"Ctrl+Ctrl+Home",
        L"Home+Ctrl", L"Home+End", L"F0", L"F25", L"F12", L"F-1", L"F1x", L"Unknown", L"Ctrl+Mouse1",
    };
    for (const auto* text : invalid)
    {
        Hotkey result{ MOD_ALT, VK_END };
        if (ParseHotkey(text, result) || result.modifiers != MOD_ALT || result.key != VK_END)
        {
            std::printf("Failed invalid shortcut: %ls\n", text);
            return 1;
        }
    }
    // The installer writes shortcuts with FormatHotkey and the host reads them with ParseHotkey.
    const struct { const wchar_t* text; const wchar_t* formatted; } roundTrip[] = {
        { L"home", L"Home" }, { L"shift+ctrl+f8", L"Ctrl+Shift+F8" }, { L"Win+Alt+pagedown", L"Alt+Win+PageDown" },
        { L"F24", L"F24" }, { L"Ctrl+7", L"Ctrl+7" }, { L"q", L"Q" },
    };
    for (const auto& test : roundTrip)
    {
        Hotkey parsed;
        if (!ParseHotkey(test.text, parsed) || FormatHotkey(parsed) != test.formatted)
        {
            std::printf("Failed to format shortcut: %ls\n", test.text);
            return 1;
        }
    }
    if (!FormatHotkey({ MOD_CONTROL, VK_F12 }).empty() || !FormatHotkey({ MOD_CONTROL, VK_LBUTTON }).empty() || !FormatHotkey({}).empty())
    {
        std::puts("Formatted an unsupported shortcut.");
        return 1;
    }
    std::puts("Hotkey tests passed.");
    return 0;
}
