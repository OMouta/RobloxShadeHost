#include "hdr.h"
#include "log.h"

#include <string>
#include <vector>

namespace
{
// DISPLAYCONFIG paths are matched to a window's monitor by comparing GDI device names ("\\.\
// DISPLAY1"), since neither DXGI nor DisplayConfig otherwise share a common identifier for the
// same physical display.
bool FindDisplayConfigPath(HWND window, DISPLAYCONFIG_PATH_INFO& found)
{
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitorInfo))
        return false;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    for (UINT32 i = 0; i < pathCount; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && monitorInfo.szDevice == std::wstring(source.viewGdiDeviceName))
        {
            found = paths[i];
            return true;
        }
    }
    // Falls back to the SDR path below, same as main, so this is worth knowing about rather than
    // silently treating an HDR display as SDR.
    Log(LogLevel::Warning, L"Could not match %ls to a display for HDR detection; capturing as SDR.", monitorInfo.szDevice);
    return false;
}
} // namespace

bool IsAdvancedColorEnabled(HWND window)
{
    DISPLAYCONFIG_PATH_INFO path{};
    if (!FindDisplayConfigPath(window, path))
        return false;

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info.header.size = sizeof(info);
    info.header.adapterId = path.targetInfo.adapterId;
    info.header.id = path.targetInfo.id;
    return DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS && info.advancedColorEnabled;
}

float GetSdrWhiteLevelNits(HWND window)
{
    DISPLAYCONFIG_PATH_INFO path{};
    if (!FindDisplayConfigPath(window, path))
        return 80.0f;

    DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
    level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    level.header.size = sizeof(level);
    level.header.adapterId = path.targetInfo.adapterId;
    level.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS || level.SDRWhiteLevel == 0)
        return 80.0f;
    // SDRWhiteLevel is in units where 1000 == 80 nits.
    return level.SDRWhiteLevel / 1000.0f * 80.0f;
}
