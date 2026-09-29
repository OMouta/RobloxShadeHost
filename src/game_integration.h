#pragma once

#include <windows.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct AutoGame
{
    std::filesystem::path executable;
    std::wstring name;
    bool enabled = true;
};

std::vector<AutoGame> LoadAutoGames(const std::filesystem::path& path);
void SaveAutoGames(const std::filesystem::path& path, std::span<const AutoGame> games);
// Games saved with a folder match that exact file. Games saved by filename, like Roblox, match it in any folder.
bool MatchesExecutable(const AutoGame& game, const std::filesystem::path& executable);

// Throws std::system_error when the process has exited or denies access.
std::filesystem::path ProcessExecutable(DWORD processId);

struct GameWindow
{
    HWND window;
    std::wstring name;
    DWORD processId;
};

std::vector<GameWindow> ListGameWindows();
bool GameWindowExists(const GameWindow& game);
void AddAutoGame(std::vector<AutoGame>& games, const GameWindow& window);
// An empty selection keeps automatic detection. A closed selection does not attach to another game.
std::optional<GameWindow> FindGameTarget(const std::optional<GameWindow>& selection, std::span<const AutoGame> games,
                                       HWND preferredWindow = nullptr);
