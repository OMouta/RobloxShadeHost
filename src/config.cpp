#include "config.h"
#include "state.h"

#include <iterator>

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
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        winrt::check_bool(WritePrivateProfileStringW(L"Input", L"ToggleKey", g.inputHotkey.c_str(), path.c_str()));
        winrt::check_bool(WritePrivateProfileStringW(L"Input", L"OverlayToggleKey", L"Ctrl+F8", path.c_str()));
    }

    wchar_t value[128]{};
    const DWORD count = GetPrivateProfileStringW(L"Input", L"ToggleKey", L"Ctrl+Home", value, static_cast<DWORD>(std::size(value)), path.c_str());
    Hotkey hotkey;
    if (count == std::size(value) - 1 || !ParseHotkey(value, hotkey))
    {
        MessageBoxW(nullptr, L"Invalid ToggleKey in RobloxShadeHost.ini. Use a key such as Ctrl+Home or F8. See the README for supported keys.",
                    L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        winrt::throw_hresult(E_INVALIDARG);
    }
    g.inputHotkey = value;
    g.indicatorText = L"Input captured | " + g.inputHotkey + L" to return to Roblox";
    const DWORD overlayCount = GetPrivateProfileStringW(L"Input", L"OverlayToggleKey", L"Ctrl+F8", value,
                                                       static_cast<DWORD>(std::size(value)), path.c_str());
    Hotkey overlayHotkey;
    if (overlayCount == std::size(value) - 1 || (overlayCount && !ParseHotkey(value, overlayHotkey)))
    {
        MessageBoxW(nullptr, L"Invalid OverlayToggleKey in RobloxShadeHost.ini. Leave it blank or use a key such as F8 or Ctrl+F8.",
                    L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        winrt::throw_hresult(E_INVALIDARG);
    }
    if (overlayHotkey.key == hotkey.key && overlayHotkey.modifiers == hotkey.modifiers)
    {
        MessageBoxW(nullptr, L"ToggleKey and OverlayToggleKey in RobloxShadeHost.ini must use different shortcuts.",
                    L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        winrt::throw_hresult(E_INVALIDARG);
    }
    g.overlayHotkey = value;
    return { hotkey, overlayHotkey };
}
