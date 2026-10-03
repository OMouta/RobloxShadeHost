#include "hdr.h"
#include "log.h"

#include <string>
#include <vector>

namespace
{
// The window's monitor and Windows' display paths only share the GDI device name, such as \\.\DISPLAY1.
bool FindDisplayPath(HWND window, DISPLAYCONFIG_PATH_INFO& found)
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
    Log(LogLevel::Info, L"Could not find %ls in Windows' display settings, so it is captured without HDR.", monitorInfo.szDevice);
    return false;
}
} // namespace

std::optional<float> HdrWhiteLevel(HWND window)
{
    DISPLAYCONFIG_PATH_INFO path{};
    if (!FindDisplayPath(window, path))
        return std::nullopt;

    // Wide color on an SDR display is advanced color too, but keeps SDR brightness, so it is captured as SDR.
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
    color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color.header.size = sizeof(color);
    color.header.adapterId = path.targetInfo.adapterId;
    color.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS || !color.advancedColorEnabled || color.wideColorEnforced)
        return std::nullopt;

    // 1000 is 80 nits, scRGB's own SDR white, which is also taken when Windows does not say.
    DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
    level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    level.header.size = sizeof(level);
    level.header.adapterId = path.targetInfo.adapterId;
    level.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS || level.SDRWhiteLevel == 0)
        return 80.0f;
    return level.SDRWhiteLevel / 1000.0f * 80.0f;
}
