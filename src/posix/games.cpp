#include "games.h"
#include "config.h"
#include "game_list.h"

#include <algorithm>
#include <cstring>
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

std::string FolderName(std::string name)
{
    for (char& c : name)
        if (static_cast<unsigned char>(c) < 32 || strchr("\\/:*?\"<>|", c))
            c = ' ';
    name.erase(0, name.find_first_not_of(' '));
    // Windows drops dots and spaces from the end of names.
    name.erase(name.find_last_not_of(". ") + 1);
    return name;
}

std::vector<AutoGame> LoadAutoGames(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
        return DefaultAutoGames();
    std::vector<AutoGame> games;
    for (GameListEntry& entry : ParseGameList(ReadFile(path)))
        games.push_back({ std::move(entry.executable), std::move(entry.name), entry.enabled });
    return games;
}

bool SaveAutoGames(const std::filesystem::path& path, std::span<const AutoGame> games)
{
    std::vector<GameListEntry> entries;
    for (const AutoGame& game : games)
        entries.push_back({ game.executable, game.name, game.enabled });
    return WriteFile(path, FormatGameList(entries));
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
