#include "log.h"
#include "config.h"

#include <sys/utsname.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace
{
constexpr size_t kMaxNotices = 40;

std::mutex mutex;
FILE* file = nullptr;
std::vector<Notice> notices;
std::filesystem::path path;

const char* LevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Ok:
        return "OK";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "INFO";
    }
}

void Write(LogLevel level, bool report, const char* format, va_list args)
{
    char text[2048];
    vsnprintf(text, sizeof(text), format, args);

    char stamp[32];
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

    std::lock_guard lock(mutex);
    for (FILE* out : { file, stderr })
        if (out)
        {
            fprintf(out, "%s %-7s %s\n", stamp, LevelName(level), text);
            fflush(out);
        }
    if (!report && level != LogLevel::Warning && level != LogLevel::Error)
        return;
    // Retries, such as a capture that keeps failing, would otherwise fill the list with one message.
    if (std::any_of(notices.begin(), notices.end(), [&](const Notice& notice) { return notice.text == text; }))
        return;
    if (notices.size() == kMaxNotices)
        notices.erase(notices.begin());
    notices.push_back({ level, text });
}
} // namespace

void InitLog()
{
    path = DataDirectory() / "Unishade.log";
    std::error_code ignored;
    std::filesystem::rename(path, DataDirectory() / "Unishade.old.log", ignored);
    file = fopen(path.c_str(), "w");

    utsname system{};
    uname(&system);
    Log(LogLevel::Info, "Unishade %s on %s %s (%s)", UNISHADE_VERSION, system.sysname, system.release, system.machine);
}

void Log(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, false, format, args);
    va_end(args);
}

void Report(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, true, format, args);
    va_end(args);
}

std::vector<Notice> Notices()
{
    std::lock_guard lock(mutex);
    return notices;
}

const std::filesystem::path& LogPath()
{
    return path;
}
