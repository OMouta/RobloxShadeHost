#include "update.h"
#include "log.h"
#include "../installer/net.h"
#include "../installer/text.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

namespace
{
// Never destroyed, since the check's thread may still run while the host exits.
struct Shared
{
    std::mutex mutex;
    Update available;
};
Shared& shared = *new Shared;

// Reads 1.2.3 from the start of text. Returns false for anything else, such as the depth-assets release.
bool ParseVersion(std::string_view text, std::array<int, 3>& version)
{
    size_t position = 0;
    for (int part = 0; part < 3; ++part)
    {
        if (position >= text.size() || text[position] < '0' || text[position] > '9')
            return false;
        version[part] = 0;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
            version[part] = version[part] * 10 + (text[position++] - '0');
        if (part < 2 && (position >= text.size() || text[position++] != '.'))
            return false;
    }
    return position == text.size();
}

// The string value after "key": in GitHub's JSON, starting at from.
std::string_view JsonString(std::string_view json, std::string_view key, size_t from)
{
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t start = json.find(pattern, from);
    if (start == std::string_view::npos)
        return {};
    start = json.find('"', json.find(':', start + pattern.size()));
    const size_t end = json.find('"', start + 1);
    return start == std::string_view::npos || end == std::string_view::npos ? std::string_view{} : json.substr(start + 1, end - start - 1);
}

void Check()
{
    std::array<int, 3> current{};
    std::array<int, 3> newest{};
    ParseVersion(ROBLOX_SHADE_HOST_VERSION, current);
    newest = current;
    std::string url;

    const std::atomic<bool> cancel = false;
    const std::string json = Fetch(L"https://api.github.com/repos/OMouta/RobloxShadeHost/releases?per_page=30", cancel);
    // Each release object starts with its "url" and lists "html_url", "tag_name" and "prerelease" after it.
    for (size_t position = json.find("\"tag_name\""); position != std::string::npos; position = json.find("\"tag_name\"", position + 1))
    {
        std::string_view tag = JsonString(json, "tag_name", position);
        std::array<int, 3> version{};
        const size_t prerelease = json.find("\"prerelease\"", position);
        const bool stable = prerelease != std::string::npos && json.compare(json.find(':', prerelease) + 1, 5, "false") == 0;
        if (tag.size() > 1 && tag[0] == 'v' && ParseVersion(tag.substr(1), version) && stable && version > newest)
        {
            newest = version;
            url = "https://github.com/OMouta/RobloxShadeHost/releases/tag/" + std::string(tag);
        }
    }
    if (newest == current)
    {
        Log(LogLevel::Info, L"RobloxShadeHost is up to date.");
        return;
    }
    const std::wstring version =
        std::to_wstring(newest[0]) + L"." + std::to_wstring(newest[1]) + L"." + std::to_wstring(newest[2]);
    Log(LogLevel::Info, L"RobloxShadeHost %ls is available.", version.c_str());
    std::lock_guard lock(shared.mutex);
    shared.available = { version, Wide(url) };
}
} // namespace

void CheckForUpdate()
{
    std::thread([] {
        try
        {
            Check();
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Info, L"Could not check for updates: %hs", e.what());
        }
    }).detach();
}

Update AvailableUpdate()
{
    std::lock_guard lock(shared.mutex);
    return shared.available;
}
