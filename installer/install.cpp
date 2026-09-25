#include "install.h"
#include "resource.h"
#include "text.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <miniz.h>

#include <algorithm>
#include <cstdarg>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
// Inno Setup's key from earlier versions of Setup, so an update replaces its entry in Windows' app list.
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{77125AF5-DF0A-485A-A633-E64FBD50E90C}_is1";
// Lists the files Setup installed, relative to the installation folder, for uninstalling.
constexpr wchar_t kManifest[] = L"RobloxShadeHost-Setup.files";
constexpr wchar_t kSetupExe[] = L"RobloxShadeHost-Setup.exe";

struct AddonInfo
{
    Addon addon;
    const char* name;
    const wchar_t* section;
    std::vector<const wchar_t*> files;
};
const AddonInfo kAddons[] = {
    { Addon::Depth, "Depth estimation", L"depth", { L"onnxruntime.dll", L"DirectML.dll", L"depth-anything-v2-small.onnx" } },
    { Addon::DLSS5, "DLSS5", L"dlss5", { L"nvngx_dlssnr.dll", L"renodx-dlss.addon64" } },
};

const struct
{
    const wchar_t* link;
    const wchar_t* target;
} kShortcuts[] = {
    { L"RobloxShadeHost.lnk", L"RobloxShadeHost.exe" },
    { L"RobloxShadeHost Setup.lnk", kSetupExe },
};

std::mutex logMutex;
std::ofstream logFile;
fs::path logPath;
fs::path movedSetup;

std::string Format(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::string SizeText(uint64_t bytes)
{
    return Format("%.1f MB", bytes / 1048576.0);
}

std::string PathText(const fs::path& path)
{
    return Utf8(path.wstring());
}

std::string SystemError(DWORD error)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = text ? text : L"error " + std::to_wstring(error);
    LocalFree(text);
    while (!message.empty() && wcschr(L"\r\n .", message.back()))
        message.pop_back();
    return Utf8(message);
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not read " + PathText(path) + ".");
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void WriteFile(const fs::path& path, std::string_view data)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(data.data(), data.size());
    file.close();
    if (!file)
        throw std::runtime_error("Could not write " + PathText(path) + ".");
}

fs::path ModulePath()
{
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

bool IsInside(const fs::path& path, const fs::path& folder)
{
    std::wstring child = path.lexically_normal().wstring();
    std::wstring parent = folder.lexically_normal().wstring();
    while (!parent.empty() && parent.back() == L'\\')
        parent.pop_back();
    return child.size() >= parent.size() && _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) == 0 &&
           (child.size() == parent.size() || child[parent.size()] == L'\\');
}

bool IsSha256(const std::string& text)
{
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::wstring IniString(const fs::path& file, const std::wstring& section, const wchar_t* key, const wchar_t* fallback = L"")
{
    wchar_t value[4096]{};
    GetPrivateProfileStringW(section.c_str(), key, fallback, value, static_cast<DWORD>(std::size(value)), file.c_str());
    return value;
}

std::vector<std::wstring> IniSections(const fs::path& file)
{
    std::vector<wchar_t> buffer(1 << 16);
    const DWORD length = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
    std::vector<std::wstring> sections;
    for (const wchar_t* name = buffer.data(); name < buffer.data() + length && *name; name += wcslen(name) + 1)
        sections.emplace_back(name);
    if (sections.empty())
        throw std::runtime_error("The download list " + PathText(file.filename()) + " is empty.");
    return sections;
}

std::string Lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

// Reports download progress as bytes, and as the part between begin and end of the whole installation.
DownloadProgress ByteProgress(Progress& progress, float begin, float end, const std::string& label = "")
{
    return [&progress, begin, end, label](uint64_t received, uint64_t total) {
        progress.Detail(label + SizeText(received) + (total ? " of " + SizeText(total) : ""));
        if (total)
            progress.Fraction(begin + (end - begin) * static_cast<float>(received) / total);
    };
}

DWORD RunHidden(std::wstring command, const fs::path& directory)
{
    STARTUPINFOW startup{ sizeof(startup) };
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
        throw std::runtime_error("Could not start the ReShade installer: " + SystemError(GetLastError()) + ".");
    CloseHandle(process.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(process.hProcess, 5 * 60 * 1000) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    else
        TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    return code;
}

// ReShade's installer writes .\reshade-shaders\Shaders\**\** as the search paths. ReShade treats a
// trailing ** as "search recursively" and then looks for a folder literally named **, so no effect or
// texture is found. Drops the extra suffix and keeps everything else, including the byte order mark.
void FixReShadeIni(const fs::path& path, bool addPresetPath)
{
    const std::string text = ReadFile(path);
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string result;
    bool changed = false;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(start, end - start);
        start = end;

        std::string content = line.substr(0, line.find_last_not_of("\r\n") + 1);
        const std::string ending = line.substr(content.size());
        if ((content.rfind("EffectSearchPaths=", 0) == 0 || content.rfind("TextureSearchPaths=", 0) == 0) && content.size() > 6 &&
            content.compare(content.size() - 6, 6, "\\**\\**") == 0)
        {
            content.resize(content.size() - 3);
            changed = true;
        }
        result += content + ending;
        if (addPresetPath && content.find("[GENERAL]") != std::string::npos)
        {
            result += (ending.empty() ? newline : "") + "PresetPath=.\\presets\\ReShadePreset.ini" + newline;
            changed = true;
        }
    }
    if (changed)
        WriteFile(path, result);
}

void ExtractZip(const std::string& data, const fs::path& destination, const std::string& name)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
        throw std::runtime_error("The download of " + name + " is not a zip file.");
    struct Closer
    {
        mz_zip_archive* zip;
        ~Closer() { mz_zip_reader_end(zip); }
    } closer{ &zip };

    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_directory)
            continue;
        const fs::path target = (destination / Wide(stat.m_filename)).lexically_normal();
        if (!IsInside(target, destination))
            throw std::runtime_error(name + " contains a file outside its own folder.");
        fs::create_directories(target.parent_path());
        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
        if (!bytes)
            throw std::runtime_error("Could not extract " + name + ".");
        const std::string_view contents(static_cast<const char*>(bytes), size);
        try
        {
            WriteFile(target, contents);
        }
        catch (...)
        {
            mz_free(bytes);
            throw;
        }
        mz_free(bytes);
    }
}

// Matches ReShade's own installer: folders named Shaders and Textures first, otherwise the shallowest
// folder with effect or image files.
void FindPackageFolders(const fs::path& directory, fs::path& shaders, fs::path& textures, fs::path& shaderFallback, fs::path& textureFallback)
{
    for (const auto& entry : fs::directory_iterator(directory))
    {
        const fs::path& path = entry.path();
        if (entry.is_directory())
        {
            if (shaders.empty() && _wcsicmp(path.filename().c_str(), L"Shaders") == 0)
                shaders = path;
            if (textures.empty() && _wcsicmp(path.filename().c_str(), L"Textures") == 0)
                textures = path;
            FindPackageFolders(path, shaders, textures, shaderFallback, textureFallback);
            continue;
        }
        const std::wstring extension = path.extension().wstring();
        const auto shallower = [&](const fs::path& current) { return current.empty() || directory.native().size() < current.native().size(); };
        if (_wcsicmp(extension.c_str(), L".fx") == 0 && shallower(shaderFallback))
            shaderFallback = directory;
        if ((_wcsicmp(extension.c_str(), L".png") == 0 || _wcsicmp(extension.c_str(), L".jpg") == 0 || _wcsicmp(extension.c_str(), L".jpeg") == 0) &&
            shallower(textureFallback))
            textureFallback = directory;
    }
}

void CopyPackageFiles(const fs::path& source, const fs::path& destination, const std::wstring& denied)
{
    fs::create_directories(destination);
    for (const auto& entry : fs::directory_iterator(source))
    {
        const fs::path name = entry.path().filename();
        if (entry.is_directory())
        {
            CopyPackageFiles(entry.path(), destination / name, denied);
            continue;
        }
        bool skip = false;
        for (const wchar_t* extension : { L".addon", L".addon32", L".addon64", L".dll", L".exe" })
            skip |= _wcsicmp(name.extension().c_str(), extension) == 0;
        for (size_t start = 0; start <= denied.size() && !skip;)
        {
            size_t end = denied.find(L',', start);
            end = end == std::wstring::npos ? denied.size() : end;
            std::wstring deniedName = denied.substr(start, end - start);
            deniedName.erase(0, deniedName.find_first_not_of(L' '));
            deniedName.erase(deniedName.find_last_not_of(L' ') + 1);
            skip = !deniedName.empty() && _wcsicmp(deniedName.c_str(), name.c_str()) == 0;
            start = end + 1;
        }
        if (!skip)
            fs::copy_file(entry.path(), destination / name, fs::copy_options::overwrite_existing);
    }
}

fs::path PackageDestination(const fs::path& files, const std::wstring& relative, const wchar_t* kind, const std::string& package)
{
    const fs::path destination = (files / relative).lexically_normal();
    if (relative.empty() || !IsInside(destination, files / L"reshade-shaders" / kind))
        throw std::runtime_error("The effect package " + package + " has an invalid install folder.");
    return destination;
}

void InstallReShade(const fs::path& work, const fs::path& files, const ReShadeRelease& release, bool presets, Progress& progress)
{
    progress.Status("Downloading ReShade " + release.version);
    const fs::path setup = work / L"ReShade-Setup.exe";
    Download(L"https://reshade.me/downloads/ReShade_Setup_" + Wide(release.version) + L"_Addon.exe", setup, "", progress.cancel,
             ByteProgress(progress, 0.0f, 0.05f));

    // ReShade's installer also leaves an empty preset and a log next to the exe, which must not replace
    // the user's files, so it runs in a folder of its own and only its DLL and settings are kept.
    progress.Status("Setting up ReShade");
    const fs::path folder = work / L"reshade";
    fs::create_directories(folder);
    fs::copy_file(files / L"RobloxShadeHost.exe", folder / L"RobloxShadeHost.exe");
    const DWORD code = RunHidden(L"\"" + setup.wstring() + L"\" --headless --api dxgi \"" + (folder / L"RobloxShadeHost.exe").wstring() + L"\"", work);
    if (code != 0 || !fs::exists(folder / L"dxgi.dll") || !fs::exists(folder / L"ReShade.ini"))
        throw std::runtime_error(Format("ReShade's installer failed (exit code %lu).", code));
    fs::copy_file(folder / L"dxgi.dll", files / L"dxgi.dll");
    fs::copy_file(folder / L"ReShade.ini", files / L"ReShade.ini");
    FixReShadeIni(files / L"ReShade.ini", presets);
    WriteFile(files / L"ReShade-LICENSE.txt", release.license);
    progress.Fraction(0.08f);
}

void InstallEffects(const fs::path& work, const fs::path& files, Progress& progress)
{
    constexpr float kBegin = 0.08f, kEnd = 0.75f;
    progress.Status("Downloading effects");
    const fs::path catalog = files / L"EffectPackages.ini";
    WriteFile(catalog, Fetch(sources.effects, progress.cancel));
    const auto sections = IniSections(catalog);
    for (size_t index = 0; index < sections.size(); ++index)
    {
        const std::wstring& section = sections[index];
        const std::string name = Utf8(IniString(catalog, section, L"PackageName", section.c_str()));
        progress.Detail(Format("%zu of %zu: ", index + 1, sections.size()) + name);
        const std::wstring url = IniString(catalog, section, L"DownloadUrl");
        if (url.rfind(L"https://", 0) != 0)
            throw std::runtime_error("The effect package " + name + " has an invalid download address.");
        const fs::path shaderDestination = PackageDestination(files, IniString(catalog, section, L"InstallPath"), L"Shaders", name);

        const fs::path extracted = work / L"package";
        fs::remove_all(extracted);
        ExtractZip(Fetch(url, progress.cancel), extracted, name);
        fs::path shaders, textures, shaderFallback, textureFallback;
        FindPackageFolders(extracted, shaders, textures, shaderFallback, textureFallback);
        if (shaders.empty())
            shaders = shaderFallback;
        if (textures.empty())
            textures = textureFallback;
        if (shaders.empty())
            throw std::runtime_error("The effect package " + name + " contains no effects.");
        CopyPackageFiles(shaders, shaderDestination, IniString(catalog, section, L"DenyEffectFiles"));
        if (!textures.empty())
            CopyPackageFiles(textures, PackageDestination(files, IniString(catalog, section, L"TextureInstallPath"), L"Textures", name), L"");
        fs::remove_all(extracted);
        SetupLog("Effect package installed: " + name);
        progress.Fraction(kBegin + (kEnd - kBegin) * (index + 1) / sections.size());
    }
}

void InstallPresets(const fs::path& work, const fs::path& files, Progress& progress)
{
    progress.Status("Downloading presets");
    const fs::path catalog = work / L"preset-downloads.ini";
    WriteFile(catalog, Fetch(sources.presets + L"/downloads.ini", progress.cancel));

    std::set<std::string> effects;
    for (const auto& entry : fs::recursive_directory_iterator(files / L"reshade-shaders" / L"Shaders"))
        if (entry.is_regular_file())
            effects.insert(Lowercase(Utf8(entry.path().filename().wstring())));

    fs::create_directories(files / L"presets");
    for (const std::wstring& file : IniSections(catalog))
    {
        const std::string name = Utf8(file);
        if (fs::path(file).filename() != file || file.find(L':') != std::wstring::npos || _wcsicmp(fs::path(file).extension().c_str(), L".ini") != 0)
            throw std::runtime_error("The preset list contains an invalid filename.");
        const std::string hash = Lowercase(Utf8(IniString(catalog, file, L"sha256")));
        if (!IsSha256(hash))
            throw std::runtime_error("The preset list has an invalid checksum for " + name + ".");
        const fs::path preset = files / L"presets" / file;
        Download(sources.presets + L"/" + file, preset, hash, progress.cancel);

        // ReShade keeps Techniques before the first section, where the INI functions cannot read it.
        const std::string text = ReadFile(preset);
        std::string techniques;
        for (size_t start = 0; start < text.size();)
        {
            size_t end = text.find('\n', start);
            end = end == std::string::npos ? text.size() : end;
            std::string line = text.substr(start, end - start);
            start = end + 1;
            line.erase(0, line.find_first_not_of(" \t\xEF\xBB\xBF"));
            line.erase(line.find_last_not_of(" \t\r") + 1);
            if (line.rfind('[', 0) == 0)
                break;
            if (Lowercase(line).rfind("techniques=", 0) == 0)
                techniques = line.substr(11);
        }
        if (techniques.empty())
            throw std::runtime_error(name + " uses no effects.");
        for (size_t start = 0; start < techniques.size();)
        {
            size_t end = techniques.find(',', start);
            end = end == std::string::npos ? techniques.size() : end;
            const std::string technique = techniques.substr(start, end - start);
            start = end + 1;
            const size_t at = technique.find('@');
            const std::string shader = at == std::string::npos ? "" : technique.substr(at + 1);
            if (shader.empty() || shader.find_first_of("\\/:") != std::string::npos || !effects.count(Lowercase(shader)))
                throw std::runtime_error(name + " needs " + (shader.empty() ? technique : shader) + ", which no effect package provides.");
        }
    }
    progress.Fraction(0.8f);
}

// Returns false and leaves a note when the add-on cannot be downloaded, so the rest still installs.
bool DownloadAddon(const fs::path& work, const fs::path& files, const AddonInfo& addon, Progress& progress)
{
    constexpr float kBegin = 0.8f, kEnd = 0.95f;
    progress.Status(std::string("Downloading ") + addon.name);
    try
    {
        const fs::path manifest = work / L"addon-downloads.ini";
        WriteFile(manifest, Fetch(addon.addon == Addon::Depth ? sources.depth : sources.dlss5, progress.cancel));
        if (IniString(manifest, addon.section, L"enabled", L"0") != L"1")
            throw std::runtime_error(std::string(addon.name) + " downloads are turned off for now.");
        for (size_t index = 0; index < addon.files.size(); ++index)
        {
            const wchar_t* file = addon.files[index];
            const std::wstring url = IniString(manifest, file, L"url");
            const std::string hash = Lowercase(Utf8(IniString(manifest, file, L"sha256")));
            if ((url.rfind(L"https://github.com/OMouta/RobloxShadeHost/releases/download/", 0) != 0 &&
                 url.rfind(L"https://huggingface.co/", 0) != 0) ||
                !IsSha256(hash))
                throw std::runtime_error(std::string("The ") + addon.name + " download list is invalid.");
            const float begin = kBegin + (kEnd - kBegin) * index / addon.files.size();
            const float end = kBegin + (kEnd - kBegin) * (index + 1) / addon.files.size();
            Download(url, files / file, hash, progress.cancel, ByteProgress(progress, begin, end, Utf8(file) + ": "));
        }
        return true;
    }
    catch (const std::exception& e)
    {
        std::error_code ignored;
        for (const wchar_t* file : addon.files)
            fs::remove(files / file, ignored);
        SetupLog(std::string(addon.name) + " skipped: " + e.what());
        progress.Note(std::string(addon.name) + " was not installed because its download failed or could not be verified. Run Setup again "
                                                "later to add it.");
        return false;
    }
}

std::set<std::wstring> ReadManifest(const fs::path& directory)
{
    std::set<std::wstring> files;
    std::ifstream file(directory / kManifest, std::ios::binary);
    for (std::string line; std::getline(file, line);)
    {
        line.erase(line.find_last_not_of("\r") + 1);
        if (!line.empty())
            files.insert(Wide(line));
    }
    return files;
}

void WriteManifest(const fs::path& directory, const std::set<std::wstring>& files)
{
    std::string text;
    std::error_code ignored;
    for (const auto& file : files)
        if (fs::exists(directory / file, ignored))
            text += Utf8(file) + "\r\n";
    WriteFile(directory / kManifest, text);
}

fs::path StartMenuFolder()
{
    PWSTR path = nullptr;
    SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &path);
    fs::path folder = path ? path : L"";
    CoTaskMemFree(path);
    return folder;
}

void CreateShortcut(const fs::path& link, const fs::path& target)
{
    ComPtr<IShellLinkW> shell;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) || FAILED(shell->SetPath(target.c_str())) ||
        FAILED(shell->SetWorkingDirectory(target.parent_path().c_str())) || FAILED(shell.As(&file)) || FAILED(file->Save(link.c_str(), TRUE)))
        throw std::runtime_error("Could not create the Start menu shortcut " + PathText(link.stem()) + ".");
}

void Register(const fs::path& directory, const std::set<std::wstring>& files)
{
    uint64_t bytes = 0;
    std::error_code error;
    for (const auto& file : files)
    {
        const uint64_t size = fs::file_size(directory / file, error);
        bytes += error ? 0 : size;
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Could not add RobloxShadeHost to Windows' list of apps.");
    const auto text = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    const auto number = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    const std::wstring setup = L"\"" + (directory / kSetupExe).wstring() + L"\"";
    text(L"DisplayName", L"RobloxShadeHost");
    text(L"DisplayVersion", Wide(ROBLOX_SHADE_HOST_VERSION));
    text(L"Publisher", L"RobloxShadeHost contributors");
    text(L"DisplayIcon", (directory / L"RobloxShadeHost.exe").wstring());
    text(L"InstallLocation", directory.wstring());
    text(L"UninstallString", setup + L" --uninstall");
    text(L"QuietUninstallString", setup + L" --uninstall --silent");
    text(L"ModifyPath", setup);
    text(L"URLInfoAbout", L"https://github.com/OMouta/RobloxShadeHost");
    text(L"HelpLink", L"https://github.com/OMouta/RobloxShadeHost/issues");
    number(L"EstimatedSize", static_cast<DWORD>(bytes / 1024));
    number(L"NoRepair", 1);
    RegCloseKey(key);
}

// The folder in Windows' app list entry, whether or not it still exists.
fs::path RegisteredDirectory()
{
    std::vector<wchar_t> location(32768);
    DWORD size = static_cast<DWORD>(location.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location.data(), &size) != ERROR_SUCCESS)
        return {};
    fs::path directory = fs::path(location.data()).lexically_normal();
    return directory.has_filename() ? directory : directory.parent_path();
}

// Calls back for each running host started from the folder.
template <typename Callback>
void ForEachHost(const fs::path& directory, Callback callback)
{
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"RobloxShadeHost", nullptr));)
    {
        DWORD id = 0;
        GetWindowThreadProcessId(window, &id);
        const std::unique_ptr<void, decltype(&CloseHandle)> process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id),
                                                                    &CloseHandle);
        if (!process)
            continue;
        wchar_t path[32768]{};
        DWORD size = static_cast<DWORD>(std::size(path));
        // A closed window cannot be the starting point of the next search, so start over.
        if (QueryFullProcessImageNameW(process.get(), 0, path, &size) && IsInside(path, directory) && callback(window, process.get()))
            window = nullptr;
    }
}

// Files cannot be replaced while the host runs from them, so ask it to quit.
void CloseHost(const fs::path& directory)
{
    ForEachHost(directory, [](HWND window, HANDLE process) {
        SetupLog("Closing RobloxShadeHost");
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("RobloxShadeHost is still running. Close it and try again.");
        return true;
    });
}

void Commit(const fs::path& files, const InstallOptions& options, Progress& progress)
{
    const fs::path& directory = options.directory;
    progress.Status("Installing to " + PathText(directory));
    progress.Detail("");
    CloseHost(directory);
    try
    {
        fs::create_directories(directory);
        // An add-on that is not selected is removed, and depth estimation and DLSS5 must not be installed together.
        if (options.reshade)
            for (const auto& addon : kAddons)
                if (addon.addon != options.addon)
                    for (const wchar_t* file : addon.files)
                        fs::remove(directory / file);

        const bool hadReShadeIni = fs::exists(directory / L"ReShade.ini");
        std::set<std::wstring> installed = ReadManifest(directory);
        for (const auto& entry : fs::recursive_directory_iterator(files))
        {
            if (!entry.is_regular_file())
                continue;
            const fs::path relative = entry.path().lexically_relative(files);
            const fs::path target = directory / relative;
            // ReShade settings and presets belong to the user once installed.
            const bool userFile = _wcsicmp(relative.c_str(), L"ReShade.ini") == 0 || _wcsicmp(relative.begin()->c_str(), L"presets") == 0;
            if (userFile && fs::exists(target))
                continue;
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
            if (!userFile)
                installed.insert(relative.wstring());
        }
        // Repairs the search paths in a ReShade.ini written by an earlier version of Setup.
        if (options.reshade && hadReShadeIni)
            FixReShadeIni(directory / L"ReShade.ini", false);

        // Inno Setup's uninstaller from earlier versions of Setup.
        fs::remove(directory / L"unins000.exe");
        fs::remove(directory / L"unins000.dat");

        if (!options.portable)
        {
            const fs::path copy = directory / kSetupExe;
            std::error_code different;
            if (!fs::equivalent(ModulePath(), copy, different))
                fs::copy_file(ModulePath(), copy, fs::copy_options::overwrite_existing);
            installed.insert(kSetupExe);
            const fs::path startMenu = StartMenuFolder();
            for (const auto& shortcut : kShortcuts)
                CreateShortcut(startMenu / shortcut.link, directory / shortcut.target);
            Register(directory, installed);
        }
        WriteManifest(directory, installed);
    }
    catch (const fs::filesystem_error& e)
    {
        throw std::runtime_error("Could not write " + PathText(e.path1()) + ": " + SystemError(e.code().value()) + ".");
    }
    progress.Fraction(1.0f);
}
} // namespace

void Progress::Status(const std::string& text)
{
    SetupLog(text);
    std::lock_guard lock(mutex);
    state.status = text;
    state.detail.clear();
    state.history.push_back(text);
}

void Progress::Detail(const std::string& text)
{
    std::lock_guard lock(mutex);
    state.detail = text;
}

void Progress::Fraction(float value)
{
    std::lock_guard lock(mutex);
    state.fraction = value;
}

void Progress::Note(const std::string& text)
{
    SetupLog(text);
    std::lock_guard lock(mutex);
    state.notes.push_back(text);
}

Progress::State Progress::Read() const
{
    std::lock_guard lock(mutex);
    return state;
}

void OpenSetupLog(const fs::path& path)
{
    std::lock_guard lock(logMutex);
    logPath = path;
    logFile.open(path, std::ios::binary | std::ios::trunc);
}

void SetupLog(const std::string& line)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::lock_guard lock(logMutex);
    logFile << Format("%04u-%02u-%02u %02u:%02u:%02u  ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond) << line << "\r\n";
    logFile.flush();
}

const fs::path& SetupLogPath()
{
    return logPath;
}

std::string_view Resource(int id)
{
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    if (!loaded)
        throw std::runtime_error("Setup is damaged. Download it again.");
    return { static_cast<const char*>(LockResource(loaded)), SizeofResource(nullptr, resource) };
}

ReShadeRelease FetchReShadeRelease(const std::atomic<bool>& cancel)
{
    const std::string page = Fetch(L"https://reshade.me/", cancel);
    const size_t end = page.find("_Addon.exe");
    const size_t start = end == std::string::npos ? end : page.rfind('_', end - 1);
    if (start == std::string::npos)
        throw std::runtime_error("Could not find ReShade's download on reshade.me.");
    ReShadeRelease release;
    release.version = page.substr(start + 1, end - start - 1);
    if (release.version.size() < 5 || release.version.size() > 30 || release.version.find_first_not_of("0123456789.") != std::string::npos)
        throw std::runtime_error("reshade.me lists an unexpected ReShade version.");
    release.license = Fetch(L"https://raw.githubusercontent.com/crosire/reshade/v" + Wide(release.version) + L"/LICENSE.md", cancel);
    if (release.license.find("Redistribution and use") == std::string::npos)
        throw std::runtime_error("Could not load the ReShade license.");
    SetupLog("Newest ReShade: " + release.version);
    return release;
}

void Install(const InstallOptions& options, const ReShadeRelease& release, Progress& progress)
{
    SetupLog("Installing RobloxShadeHost " ROBLOX_SHADE_HOST_VERSION " to " + PathText(options.directory));
    const fs::path work = fs::temp_directory_path() / (L"RobloxShadeHost-Setup-" + std::to_wstring(GetCurrentProcessId()));
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{ work };
    const fs::path files = work / L"files";
    try
    {
        fs::remove_all(work);
        fs::create_directories(files);
    }
    catch (const fs::filesystem_error& e)
    {
        throw std::runtime_error("Could not prepare " + PathText(work) + ": " + SystemError(e.code().value()) + ".");
    }

    WriteFile(files / L"RobloxShadeHost.exe", Resource(IDR_HOST));
    WriteFile(files / L"LICENSE", Resource(IDR_LICENSE));
    WriteFile(files / L"CREDITS.txt", Resource(IDR_CREDITS));
    if (options.reshade)
    {
        InstallReShade(work, files, release, options.presets, progress);
        InstallEffects(work, files, progress);
        if (options.presets)
            InstallPresets(work, files, progress);
        for (const auto& addon : kAddons)
            if (addon.addon == options.addon)
                DownloadAddon(work, files, addon, progress);
    }
    Commit(files, options, progress);
    SetupLog("Installation finished.");
}

void Uninstall(const fs::path& directory, bool deleteUserFiles)
{
    SetupLog("Uninstalling from " + PathText(directory));
    CloseHost(directory);

    // A running exe cannot be deleted, but it can be moved on the same drive.
    const fs::path self = ModulePath();
    if (IsInside(self, directory))
    {
        const std::wstring name = L"RobloxShadeHost-Setup-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
        for (const fs::path& target : { fs::temp_directory_path() / name, directory.parent_path() / name })
            if (MoveFileExW(self.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                movedSetup = target;
                break;
            }
    }

    // The shortcuts and the app list entry belong to the registered installation, not to a portable copy.
    std::error_code ignored;
    const fs::path registered = RegisteredDirectory();
    if (!registered.empty() && IsInside(registered, directory) && IsInside(directory, registered))
    {
        const fs::path startMenu = StartMenuFolder();
        for (const auto& shortcut : kShortcuts)
            fs::remove(startMenu / shortcut.link, ignored);
        RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    }

    if (deleteUserFiles)
    {
        std::error_code error;
        fs::remove_all(directory, error);
        if (error)
            throw std::runtime_error("Could not delete " + PathText(directory) + ": " + SystemError(error.value()) + ".");
        SetupLog("Uninstalled.");
        return;
    }

    for (const auto& file : ReadManifest(directory))
    {
        std::error_code error;
        fs::remove(directory / file, error);
        if (error)
            SetupLog("Could not delete " + Utf8(file) + ": " + SystemError(error.value()));
    }
    for (const wchar_t* file : { kManifest, L"RobloxShadeHost.log", L"RobloxShadeHost.old.log" })
        fs::remove(directory / file, ignored);

    // Deepest folders first, so parents are empty by the time they are tried. Folders with user files stay.
    std::vector<fs::path> folders;
    std::error_code error;
    for (auto entry = fs::recursive_directory_iterator(directory, error); !error && entry != fs::recursive_directory_iterator();
         entry.increment(error))
        if (entry->is_directory())
            folders.push_back(entry->path());
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    folders.push_back(directory);
    for (const auto& folder : folders)
        fs::remove(folder, ignored);
    SetupLog("Uninstalled.");
}

void DeleteMovedSetup()
{
    if (movedSetup.empty())
        return;
    // Gives Setup two seconds to exit before deleting it.
    std::wstring command = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + movedSetup.wstring() + L"\"";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

std::optional<Installation> FindInstallation()
{
    const fs::path directory = RegisteredDirectory();
    std::error_code ignored;
    if (directory.empty() || !fs::exists(directory / L"RobloxShadeHost.exe", ignored))
        return std::nullopt;
    wchar_t version[64]{};
    DWORD size = sizeof(version);
    RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, version, &size);
    return Installation{ directory, Utf8(version) };
}

Addon InstalledAddon(const fs::path& directory)
{
    std::error_code ignored;
    for (const auto& addon : kAddons)
        for (const wchar_t* file : addon.files)
            if (fs::exists(directory / file, ignored))
                return addon.addon;
    return Addon::None;
}

std::wstring ReadShortcut(const fs::path& directory, const wchar_t* name, const wchar_t* fallback)
{
    wchar_t value[128]{};
    GetPrivateProfileStringW(L"Input", name, fallback, value, static_cast<DWORD>(std::size(value)), (directory / L"RobloxShadeHost.ini").c_str());
    return value;
}

void WriteShortcuts(const fs::path& directory, const std::wstring& toggleKey, const std::wstring& overlayToggleKey)
{
    const std::wstring ini = (directory / L"RobloxShadeHost.ini").wstring();
    if (!WritePrivateProfileStringW(L"Input", L"ToggleKey", toggleKey.c_str(), ini.c_str()) ||
        !WritePrivateProfileStringW(L"Input", L"OverlayToggleKey", overlayToggleKey.c_str(), ini.c_str()))
        throw std::runtime_error("Could not save " + Utf8(ini) + ": " + SystemError(GetLastError()) + ".");
    SetupLog("Shortcuts saved: ToggleKey=" + Utf8(toggleKey) + ", OverlayToggleKey=" + Utf8(overlayToggleKey));
}

bool HostRunning(const fs::path& directory)
{
    bool running = false;
    ForEachHost(directory, [&](HWND, HANDLE) {
        running = true;
        return false;
    });
    return running;
}

void LaunchHost(const fs::path& directory)
{
    ShellExecuteW(nullptr, L"open", (directory / L"RobloxShadeHost.exe").c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
}
