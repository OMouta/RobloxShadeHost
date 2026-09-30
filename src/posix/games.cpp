#include "games.h"
#include "config.h"
#include "ini_text.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <strings.h>

namespace
{
// The last part of a Unix or Windows path.
std::string BaseName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
} // namespace

std::vector<AutoGame> DefaultAutoGames()
{
#ifdef __APPLE__
    return { { "RobloxPlayer", "Roblox" } };
#else
    // Sober runs Roblox's Android version natively. Roblox's Windows version also runs through Wine.
    return { { "sober", "Roblox" }, { "RobloxPlayerBeta.exe", "Roblox" } };
#endif
}

bool MatchesProcess(const AutoGame& game, const std::string& executable, const std::string& command)
{
    if (game.executable.find('/') != std::string::npos)
        return executable == game.executable;
    return !strcasecmp(BaseName(executable).c_str(), game.executable.c_str()) ||
           (!command.empty() && !strcasecmp(BaseName(command).c_str(), game.executable.c_str()));
}

std::vector<AutoGame> LoadAutoGames(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
        return DefaultAutoGames();
    IniText ini(ReadFile(path));
    std::string countText;
    size_t count = 0;
    if (!ini.Get("Games", "Count", countText))
        throw std::runtime_error("The saved game list has no game count");
    const auto parsed = std::from_chars(countText.data(), countText.data() + countText.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != countText.data() + countText.size())
        throw std::runtime_error("The saved game count is invalid");
    std::vector<AutoGame> games;
    for (size_t i = 0; i < count; ++i)
    {
        const std::string section = "Game" + std::to_string(i);
        AutoGame game;
        std::string enabled;
        if (!ini.Get(section, "Executable", game.executable) || game.executable.empty() || !ini.Get(section, "Name", game.name) ||
            game.name.empty() || !ini.Get(section, "Enabled", enabled) || (enabled != "0" && enabled != "1"))
            throw std::runtime_error("A saved game entry is invalid");
        game.enabled = enabled == "1";
        games.push_back(std::move(game));
    }
    return games;
}

bool SaveAutoGames(const std::filesystem::path& path, std::span<const AutoGame> games)
{
    IniText ini("");
    ini.Set("Games", "Count", std::to_string(games.size()));
    for (size_t i = 0; i < games.size(); ++i)
    {
        const std::string section = "Game" + std::to_string(i);
        ini.Set(section, "Executable", games[i].executable);
        ini.Set(section, "Name", games[i].name);
        ini.Set(section, "Enabled", games[i].enabled ? "1" : "0");
    }
    return WriteFile(path, ini.Text());
}

bool AddAutoGame(std::vector<AutoGame>& games, const platform::Window& window)
{
    const std::string executable = platform::ProcessExecutable(window.pid);
    if (executable.empty())
        return false;
    const std::string command = platform::ProcessCommand(window.pid);
    for (AutoGame& game : games)
        if (MatchesProcess(game, executable, command))
        {
            game.enabled = true;
            return true;
        }
    // Wine and Proton games are saved by their .exe, since the executable is Wine's own.
    const std::string windowsExe = BaseName(command);
    const bool wine = windowsExe.size() > 4 && !strcasecmp(windowsExe.c_str() + windowsExe.size() - 4, ".exe");
    std::string name = window.title;
    std::replace(name.begin(), name.end(), '\r', ' ');
    std::replace(name.begin(), name.end(), '\n', ' ');
    games.push_back({ wine ? windowsExe : executable, name.empty() ? BaseName(executable) : name });
    return true;
}

std::optional<platform::Window> FindGameTarget(const std::optional<platform::Window>& selection, std::span<const AutoGame> games,
                                              platform::WindowId preferred)
{
    if (selection)
        return platform::WindowExists(*selection) ? selection : std::nullopt;
    if (std::none_of(games.begin(), games.end(), [](const AutoGame& game) { return game.enabled; }))
        return std::nullopt;

    std::vector<std::pair<int, size_t>> processes;
    for (const platform::Process& process : platform::ListProcesses())
        for (size_t i = 0; i < games.size(); ++i)
            if (games[i].enabled && MatchesProcess(games[i], process.executable, process.command))
            {
                processes.emplace_back(process.pid, i);
                break;
            }
    if (processes.empty())
        return std::nullopt;

    std::optional<platform::Window> first;
    for (platform::Window window : platform::ListWindows())
        for (const auto& [pid, index] : processes)
            if (window.pid == pid)
            {
                window.title = games[index].name;
                if (window.id == preferred)
                    return window;
                if (!first)
                    first = window;
                break;
            }
    return first;
}
