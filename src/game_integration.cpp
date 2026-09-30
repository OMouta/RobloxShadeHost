#include "game_integration.h"
#include "game_list.h"
#include "../installer/text.h"

#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

fs::path ProcessExecutable(DWORD processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        throw std::system_error(GetLastError(), std::system_category(), "Could not identify the game executable");
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    const DWORD error = GetLastError();
    CloseHandle(process);
    if (!success)
        throw std::system_error(error, std::system_category(), "Could not identify the game executable");
    path.resize(size);
    return path;
}

bool MatchesExecutable(const AutoGame& game, const fs::path& executable)
{
    return _wcsicmp((game.executable.has_parent_path() ? executable : executable.filename()).c_str(), game.executable.c_str()) == 0;
}

std::vector<AutoGame> LoadAutoGames(const fs::path& path)
{
    if (!fs::exists(path))
        return { { L"RobloxPlayerBeta.exe", L"Roblox" } };
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read the saved game list");
    std::vector<AutoGame> games;
    for (const GameListEntry& entry : ParseGameList(std::string{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() }))
        games.push_back({ Wide(entry.executable), Wide(entry.name), entry.enabled });
    return games;
}

void SaveAutoGames(const fs::path& path, std::span<const AutoGame> games)
{
    std::vector<GameListEntry> entries;
    for (const AutoGame& game : games)
        entries.push_back({ Utf8(game.executable.wstring()), Utf8(game.name), game.enabled });
    const std::string text = FormatGameList(entries);
    const fs::path temporary = path.wstring() + L".tmp";
    try
    {
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(temporary, std::ios::binary | std::ios::trunc);
        output << text;
        output.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Could not save the game list");
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

void AddAutoGame(std::vector<AutoGame>& games, const GameWindow& window)
{
    if (!GameWindowExists(window))
        throw std::runtime_error("The game window has closed. Open it again before adding it");
    const fs::path executable = ProcessExecutable(window.processId);
    for (AutoGame& game : games)
        if (MatchesExecutable(game, executable))
        {
            game.enabled = true;
            return;
        }
    std::wstring name = window.name;
    std::replace(name.begin(), name.end(), L'\r', L' ');
    std::replace(name.begin(), name.end(), L'\n', L' ');
    games.push_back({ executable, std::move(name) });
}

std::vector<GameWindow> ListGameWindows()
{
    std::vector<GameWindow> windows;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process == GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
                (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                return TRUE;
            RECT client{};
            if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
                return TRUE;
            const int length = GetWindowTextLengthW(window);
            if (!length)
                return TRUE;
            std::wstring name(static_cast<size_t>(length) + 1, L'\0');
            name.resize(GetWindowTextW(window, name.data(), static_cast<int>(name.size())));
            if (!name.empty())
                reinterpret_cast<std::vector<GameWindow>*>(param)->push_back({ window, std::move(name), process });
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    std::sort(windows.begin(), windows.end(), [](const GameWindow& a, const GameWindow& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return windows;
}

std::optional<GameWindow> FindGameTarget(const std::optional<GameWindow>& selection, std::span<const AutoGame> games, HWND preferredWindow)
{
    if (selection)
        return GameWindowExists(*selection) ? selection : std::nullopt;
    if (std::none_of(games.begin(), games.end(), [](const AutoGame& game) { return game.enabled; }))
        return std::nullopt;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return std::nullopt;
    std::vector<std::pair<DWORD, size_t>> processes;
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        for (size_t i = 0; i < games.size(); ++i)
        {
            const AutoGame& game = games[i];
            if (!game.enabled || _wcsicmp(entry.szExeFile, game.executable.filename().c_str()) != 0)
                continue;
            if (game.executable.has_parent_path())
            {
                try
                {
                    if (_wcsicmp(ProcessExecutable(entry.th32ProcessID).c_str(), game.executable.c_str()) != 0)
                        continue;
                }
                catch (const std::system_error&)
                {
                    // A process can exit or deny metadata access while its window is still listed.
                    continue;
                }
            }
            processes.emplace_back(entry.th32ProcessID, i);
            break;
        }
    CloseHandle(snapshot);
    if (processes.empty())
        return std::nullopt;
    std::optional<GameWindow> first;
    for (GameWindow window : ListGameWindows())
        for (const auto& [process, index] : processes)
            if (window.processId == process)
            {
                window.name = games[index].name;
                if (window.window == preferredWindow)
                    return window;
                if (!first)
                    first = window;
                break;
            }
    return first;
}

bool GameWindowExists(const GameWindow& game)
{
    DWORD process = 0;
    return IsWindow(game.window) && GetWindowThreadProcessId(game.window, &process) && process == game.processId;
}
