#pragma once

#include "net.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class Addon
{
    None,
    Depth,
    DLSS5,
};

struct InstallOptions
{
    std::filesystem::path directory;
    bool reshade = true;
    bool presets = true;
    Addon addon = Addon::None;
    // Leaves out the Start menu shortcuts, the entry in Windows' app list and the copy of Setup.
    bool portable = false;
};

// Download locations. Tests point them elsewhere from the command line.
struct Sources
{
    std::wstring effects = L"https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini";
    std::wstring presets = L"https://raw.githubusercontent.com/OMouta/RobloxShadeHost/main/presets";
    std::wstring dlss5 = L"https://github.com/OMouta/RobloxShadeHost/releases/download/dlss5-assets/downloads.ini";
    std::wstring depth = L"https://github.com/OMouta/RobloxShadeHost/releases/download/depth-assets/downloads.ini";
};
inline Sources sources;

struct ReShadeRelease
{
    std::string version;
    std::string license;
};

// Written by the install thread and read by the window.
class Progress
{
public:
    struct State
    {
        std::string status;
        std::string detail;
        float fraction = 0;
        std::vector<std::string> history;
        std::vector<std::string> notes;
    };

    std::atomic<bool> cancel = false;

    // A new step. Also written to the setup log.
    void Status(const std::string& text);
    void Detail(const std::string& text);
    void Fraction(float value);
    // Something the user should know at the end, such as a skipped add-on. Also written to the setup log.
    void Note(const std::string& text);
    State Read() const;

private:
    mutable std::mutex mutex;
    State state;
};

void OpenSetupLog(const std::filesystem::path& path);
void SetupLog(const std::string& line);
const std::filesystem::path& SetupLogPath();

// A resource embedded in Setup, such as the host exe.
std::string_view Resource(int id);

// Finds the newest ReShade on reshade.me and downloads its license.
ReShadeRelease FetchReShadeRelease(const std::atomic<bool>& cancel);

// Downloads everything into a temporary folder first, so a failed or cancelled download leaves the
// installation folder untouched. Throws std::runtime_error or Cancelled.
void Install(const InstallOptions& options, const ReShadeRelease& release, Progress& progress);

// Removes what Setup installed. ReShade.ini, presets and RobloxShadeHost.ini stay unless deleteUserFiles is set.
// When Setup runs from the folder, it moves its own exe out first; call DeleteMovedSetup before exiting.
void Uninstall(const std::filesystem::path& directory, bool deleteUserFiles);
void DeleteMovedSetup();

struct Installation
{
    std::filesystem::path directory;
    std::string version;
};
std::optional<Installation> FindInstallation();

// The add-on whose files are in the folder, to preselect it.
Addon InstalledAddon(const std::filesystem::path& directory);

std::wstring ReadShortcut(const std::filesystem::path& directory, const wchar_t* name, const wchar_t* fallback);
void WriteShortcuts(const std::filesystem::path& directory, const std::wstring& toggleKey, const std::wstring& overlayToggleKey);

bool HostRunning(const std::filesystem::path& directory);
void LaunchHost(const std::filesystem::path& directory);
