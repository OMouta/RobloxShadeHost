#include "setup_check.h"
#include "addon.h"
#include "config.h"
#include "log.h"
#include "state.h"

#include <reshade.hpp>

#include <filesystem>
#include <initializer_list>
#include <vector>

namespace
{
bool Exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring FileVersion(HMODULE module)
{
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    DWORD handle = 0;
    std::vector<char> data(GetFileVersionInfoSizeW(path, &handle));
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (data.empty() || !GetFileVersionInfoW(path, 0, static_cast<DWORD>(data.size()), data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length))
        return L"(unknown version)";
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(info->dwFileVersionLS));
}

// Returns whether any of the add-on's files are present.
bool CheckAddon(const std::wstring& directory, const wchar_t* name, std::initializer_list<const wchar_t*> files)
{
    std::wstring missing;
    size_t found = 0;
    for (const wchar_t* file : files)
    {
        if (Exists(directory + file))
            ++found;
        else
            missing += (missing.empty() ? L"" : L", ") + std::wstring(file);
    }
    if (!found)
        Log(LogLevel::Info, L"%ls is not installed", name);
    else if (missing.empty())
        Log(LogLevel::Ok, L"%ls is installed", name);
    else
        Log(LogLevel::Warning, L"%ls is missing %ls. Run RobloxShadeHost Setup again and select %ls.", name, missing.c_str(), name);
    return found > 0;
}
} // namespace

void CheckSetup()
{
    const std::wstring directory = ExeDirectory();

    const HMODULE reshade = reshade::internal::get_reshade_module_handle();
    if (reshade && AddonRegistered())
        Log(LogLevel::Ok, L"ReShade %ls", FileVersion(reshade).c_str());
    else if (reshade)
        Log(LogLevel::Warning, L"ReShade %ls is running without the RobloxShadeHost add-on, so %ls only captures input. Open the menu with "
                               L"ReShade's own key, or enable RobloxShadeHost in ReShade's Add-ons tab and restart.",
            FileVersion(reshade).c_str(), g.inputHotkey.c_str());
    else if (Exists(directory + L"d3d9.dll") || Exists(directory + L"opengl32.dll"))
        Log(LogLevel::Error, L"ReShade is set up for DirectX 9 or OpenGL, which RobloxShadeHost does not use, so no effects will show. "
                             L"Run RobloxShadeHost Setup again.");
    else
        Log(LogLevel::Error, L"ReShade was not found next to RobloxShadeHost.exe, so no effects will show. Run RobloxShadeHost Setup again.");

    if (reshade)
    {
        size_t effects = 0;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator entry(directory + L"reshade-shaders\\Shaders", error), end; !error && entry != end;
             entry.increment(error))
            if (_wcsicmp(entry->path().extension().c_str(), L".fx") == 0)
                ++effects;
        if (effects)
            Log(LogLevel::Ok, L"%zu effects in reshade-shaders\\Shaders", effects);
        else
            Log(LogLevel::Warning, L"No effects found in reshade-shaders\\Shaders. Run RobloxShadeHost Setup again to download them.");
    }

    DXGI_ADAPTER_DESC adapter{};
    winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
    if (SUCCEEDED(g.device.as<IDXGIDevice>()->GetAdapter(dxgiAdapter.put())))
        dxgiAdapter->GetDesc(&adapter);
    Log(LogLevel::Info, L"GPU: %ls", adapter.Description);

    const bool depth = CheckAddon(directory, L"Depth estimation", { L"depth-anything-v2-small.onnx", L"onnxruntime.dll", L"DirectML.dll" });
    const bool dlss = CheckAddon(directory, L"DLSS5", { L"renodx-dlss.addon64", L"nvngx_dlssnr.dll" });
    if (depth && dlss)
        Log(LogLevel::Warning, L"Depth estimation and DLSS5 do not work together. Run RobloxShadeHost Setup again and pick one.");
    constexpr UINT kNvidia = 0x10DE;
    if (dlss && adapter.VendorId != kNvidia)
        Log(LogLevel::Warning, L"DLSS5 needs an NVIDIA RTX GPU, but RobloxShadeHost is running on %ls. "
                               L"See https://github.com/OMouta/RobloxShadeHost/blob/main/DLSS5-README.md",
            adapter.Description);

    if (Exists(directory + L"RobloxPlayerBeta.exe"))
        Log(LogLevel::Warning, L"RobloxShadeHost is inside Roblox's folder, which Roblox replaces when it updates. Install it to its own folder.");
}
