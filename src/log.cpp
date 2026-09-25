#include "log.h"
#include "config.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
std::mutex mutex;
HANDLE file = INVALID_HANDLE_VALUE;
std::wstring path;
bool colors = false;

std::string Utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

void WriteToFile(const std::string& text)
{
    DWORD written = 0;
    if (file != INVALID_HANDLE_VALUE)
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}
} // namespace

void InitLog()
{
    DWORD mode = 0;
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    colors = GetConsoleMode(console, &mode) && SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);

    path = ExeDirectory() + L"RobloxShadeHost.log";
    MoveFileExW(path.c_str(), (ExeDirectory() + L"RobloxShadeHost.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    // GetVersionEx reports Windows 8 to programs without a compatibility manifest.
    RTL_OSVERSIONINFOW version{ sizeof(version) };
    using RtlGetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&version);
    char header[512];
    snprintf(header, sizeof(header), "RobloxShadeHost %s on Windows %lu.%lu.%lu\r\n", ROBLOX_SHADE_HOST_VERSION,
             version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    WriteToFile(header + std::string("Folder: ") + Utf8(ExeDirectory().c_str()) + "\r\n\r\n");

    if (file == INVALID_HANDLE_VALUE)
        Log(LogLevel::Warning, L"Could not create %ls (error %lu). Messages are only shown here.", path.c_str(), GetLastError());
}

void Log(LogLevel level, const wchar_t* format, ...)
{
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    const std::string message = Utf8(buffer);

    static constexpr struct
    {
        const char* name;
        const char* color;
    } kLevels[] = { { "", "" }, { "ok", "\x1b[32m" }, { "warning", "\x1b[33m" }, { "error", "\x1b[91m" } };
    const auto& tag = kLevels[static_cast<int>(level)];

    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[64];

    std::lock_guard lock(mutex);
    if (colors)
        std::printf("\x1b[90m%02u:%02u:%02u\x1b[0m  %s%-7s\x1b[0m  %s\n", time.wHour, time.wMinute, time.wSecond, tag.color, tag.name,
                    message.c_str());
    else
        std::printf("%02u:%02u:%02u  %-7s  %s\n", time.wHour, time.wMinute, time.wSecond, tag.name, message.c_str());
    std::fflush(stdout);

    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u  %-7s  ", time.wYear, time.wMonth, time.wDay, time.wHour,
             time.wMinute, time.wSecond, time.wMilliseconds, tag.name);
    WriteToFile(line + message + "\r\n");
}

const std::wstring& LogPath()
{
    return path;
}
