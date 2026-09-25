#include "config.h"
#include "log.h"
#include "state.h"

#include <iterator>

namespace
{
constexpr wchar_t kDefaultToggleKey[] = L"Ctrl+Home";
constexpr wchar_t kDefaultOverlayToggleKey[] = L"Ctrl+F8";

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
    Log(LogLevel::Warning, L"%ls=%ls in RobloxShadeHost.ini is not a supported shortcut. Using %ls. See the README for supported keys.",
        name, value, fallback);
    ParseHotkey(fallback, hotkey);
    return hotkey;
}
} // namespace

std::wstring ExeDirectory()
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    const std::wstring path(executable, length);
    return path.substr(0, path.find_last_of(L"\\/") + 1);
}

InputHotkeys LoadInputHotkeys()
{
    const std::wstring path = ExeDirectory() + L"RobloxShadeHost.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !(WritePrivateProfileStringW(L"Input", L"ToggleKey", kDefaultToggleKey, path.c_str()) &&
          WritePrivateProfileStringW(L"Input", L"OverlayToggleKey", kDefaultOverlayToggleKey, path.c_str())))
        Log(LogLevel::Warning, L"Could not create %ls. Using the default shortcuts.", path.c_str());

    InputHotkeys hotkeys{ ReadHotkey(path, L"ToggleKey", kDefaultToggleKey, false),
                          ReadHotkey(path, L"OverlayToggleKey", kDefaultOverlayToggleKey, true) };
    if (hotkeys.overlay.key == hotkeys.input.key && hotkeys.overlay.modifiers == hotkeys.input.modifiers)
    {
        Log(LogLevel::Warning, L"ToggleKey and OverlayToggleKey in RobloxShadeHost.ini are both %ls. The overlay shortcut is off until you change one.",
            FormatHotkey(hotkeys.input).c_str());
        hotkeys.overlay = {};
    }
    g.inputHotkey = FormatHotkey(hotkeys.input);
    g.overlayHotkey = FormatHotkey(hotkeys.overlay);
    g.indicatorText = L"Input captured | " + g.inputHotkey + L" to return to Roblox";
    return hotkeys;
}
