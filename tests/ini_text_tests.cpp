#include "../src/ini_text.h"
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

    // A key for a missing section goes under that section's new header, not the section before it.
    IniText ini("[GENERAL]\r\nPresetPath=a.ini\r\n\r\n[OVERLAY]\r\nTutorialProgress=0\r\n");
    ini.Set("STYLE", "Font", "segoeui.ttf");
    ini.Set("OVERLAY", "TutorialProgress", "4");
    ini.Set("GENERAL", "NoReloadOnInit", "0");
    ok &= Check(ini.Text() == "[GENERAL]\r\nPresetPath=a.ini\r\nNoReloadOnInit=0\r\n\r\n[OVERLAY]\r\nTutorialProgress=4\r\n\r\n[STYLE]\r\nFont=segoeui.ttf\r\n",
                "sections and keys land where they belong");

    std::string value;
    ok &= Check(ini.Get("STYLE", "Font", value) && value == "segoeui.ttf", "reads a key back");
    ok &= Check(!ini.Get("OVERLAY", "Font", value), "a key belongs to one section");
    ok &= Check(!ini.Get("GENERAL", "Preset", value), "a key must match whole");

    // The byte order mark and line endings stay as they were.
    IniText unix("\xEF\xBB\xBF[ADDON]\nDisabledAddons=A\n");
    unix.Set("ADDON", "DisabledAddons", "");
    ok &= Check(unix.Text() == "\xEF\xBB\xBF[ADDON]\nDisabledAddons=\n", "keeps the byte order mark and LF endings");

    IniText empty("");
    empty.Set("OVERLAY", "TutorialProgress", "4");
    ok &= Check(empty.Text() == "[OVERLAY]\r\nTutorialProgress=4\r\n", "fills an empty file");
    ok &= Check(!IniText("[A]\r\nB=1\r\n").Changed(), "unchanged until Set");

    return ok ? 0 : 1;
}
