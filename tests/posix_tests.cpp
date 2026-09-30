// Tests for the macOS and Linux host's parts that need no window: shortcuts, presets, the saved game list and
// the checksum Setup verifies presets with.

#include "config.h"
#include "games.h"
#include "hotkeys.h"
#include "preset.h"
#include "setup.h"

#include <cstdio>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}
} // namespace

int main()
{
    bool ok = true;

    // Shortcuts are written as on Windows and read back the same.
    Hotkey hotkey;
    ok &= Check(ParseHotkey("Ctrl+F8", hotkey) && hotkey.modifiers == kCtrl && hotkey.key == ImGuiKey_F8, "reads Ctrl+F8");
    ok &= Check(ParseHotkey("ctrl + shift + pagedown", hotkey) && hotkey.modifiers == (kCtrl | kShift) && hotkey.key == ImGuiKey_PageDown,
                "reads names without case and with spaces");
    ok &= Check(ParseHotkey("Cmd+Shift+U", hotkey) && hotkey.modifiers == (kSuper | kShift) && hotkey.key == ImGuiKey_U, "Cmd is the Super key");
    ok &= Check(ParseHotkey("Win+1", hotkey) && hotkey.modifiers == kSuper && hotkey.key == ImGuiKey_1, "Win is the Super key too");
    ok &= Check(ParseHotkey("Option+Left", hotkey) && hotkey.modifiers == kAlt && hotkey.key == ImGuiKey_LeftArrow, "Option is Alt");
    ok &= Check(!ParseHotkey("Ctrl", hotkey), "a modifier alone is no shortcut");
    ok &= Check(!ParseHotkey("Ctrl+Ctrl+A", hotkey), "a modifier counts once");
    ok &= Check(!ParseHotkey("A+B", hotkey), "one key per shortcut");
    ok &= Check(!ParseHotkey("F25", hotkey) && !ParseHotkey("Ctrl+", hotkey) && !ParseHotkey("", hotkey), "rejects what is not a key");
    for (const Shortcut& shortcut : kShortcuts)
    {
        Hotkey parsed, again;
        ok &= Check(ParseHotkey(shortcut.fallback, parsed) && ParseHotkey(FormatHotkey(parsed), again) && again == parsed,
                    "every default shortcut survives writing and reading");
    }

    // Presets keep keys before the first section and the order of everything else.
    const std::string text = "PreprocessorDefinitions=A=1,B\r\nTechniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\r\n\r\n"
                             "[Curves.fx]\r\nContrast=0.650000\r\n\r\n[LumaSharpen.fx]\r\nsharp_strength=0.800000\r\n";
    PresetIni preset(text);
    std::string value;
    ok &= Check(preset.Get("", "Techniques", value) && PresetIni::Split(value).size() == 2, "reads the technique list");
    ok &= Check(preset.Get("Curves.fx", "Contrast", value) && value == "0.650000", "reads an effect's value");
    ok &= Check(!preset.Get("", "Contrast", value), "values belong to their section");
    ok &= Check(preset.SectionNames() == std::vector<std::string>{ "Curves.fx", "LumaSharpen.fx" }, "lists sections in order");
    preset.Set("Curves.fx", "Contrast", "0.500000");
    preset.Set("Vibrance.fx", "Vibrance", "0.150000");
    preset.Set("", "TechniqueSorting", "LumaSharpen@LumaSharpen.fx,Curves@Curves.fx");
    ok &= Check(preset.Text() ==
                    "PreprocessorDefinitions=A=1,B\nTechniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\n"
                    "TechniqueSorting=LumaSharpen@LumaSharpen.fx,Curves@Curves.fx\n\n[Curves.fx]\nContrast=0.500000\n\n"
                    "[LumaSharpen.fx]\nsharp_strength=0.800000\n\n[Vibrance.fx]\nVibrance=0.150000\n",
                "writes changes in place and new keys at the end of their section");
    ok &= Check(PresetIni(PresetIni("\xEF\xBB\xBFTechniques=A@A.fx\n").Text()).Get("", "Techniques", value) && value == "A@A.fx",
                "reads presets with a byte order mark");

    const auto definitions = ParseDefinitions("A=1,B,,C=x=y");
    ok &= Check(definitions.size() == 3 && definitions[0] == std::pair<std::string, std::string>{ "A", "1" } && definitions[1].second.empty() &&
                    definitions[2].second == "x=y",
                "reads preprocessor definitions");
    ok &= Check(FormatDefinitions(definitions) == "A=1,B,C=x=y", "writes preprocessor definitions back");

    // Saved games match by filename anywhere, by full path exactly, and Wine games by the .exe they run.
    const AutoGame roblox{ "RobloxPlayer", "Roblox" };
    ok &= Check(MatchesProcess(roblox, "/Applications/Roblox.app/Contents/MacOS/RobloxPlayer", ""), "matches a filename in any folder");
    ok &= Check(MatchesProcess(roblox, "/opt/robloxplayer", ""), "compares filenames without case");
    ok &= Check(!MatchesProcess(roblox, "/usr/bin/RobloxPlayerBeta", ""), "a filename must match whole");
    const AutoGame wine{ "RobloxPlayerBeta.exe", "Roblox" };
    ok &= Check(MatchesProcess(wine, "/usr/bin/wine64-preloader", "C:\\Program Files\\Roblox\\RobloxPlayerBeta.exe"),
                "matches the Windows executable a Wine process runs");
    const AutoGame path{ "/usr/games/game", "Game" };
    ok &= Check(MatchesProcess(path, "/usr/games/game", "") && !MatchesProcess(path, "/opt/game", "game"), "a saved path must match exactly");

    // SHA-256, from the standard's own examples.
    ok &= Check(Sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "hashes nothing");
    ok &= Check(Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "hashes abc");
    ok &= Check(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                "hashes across two blocks");
    ok &= Check(Sha256(std::string(1000, 'a')).size() == 64, "hashes long input");

    std::printf(ok ? "All tests passed.\n" : "Some tests failed.\n");
    return ok ? 0 : 1;
}
