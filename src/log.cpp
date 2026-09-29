#include "log.h"
#include "config.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
constexpr size_t kMaxNotices = 40;

// Never destroyed, since other threads, such as the update check, may still log while the host exits.
struct Shared
{
    std::mutex mutex;
    std::vector<Notice> notices;
};
Shared& shared = *new Shared;
HANDLE file = INVALID_HANDLE_VALUE;
std::wstring path;
std::atomic<unsigned> noticeVersion = 0;

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

void Write(LogLevel level, bool notice, const wchar_t* format, va_list args)
{
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);

    static constexpr const char* kNames[] = { "", "ok", "warning", "error" };
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[64];
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u  %-7s  ", time.wYear, time.wMonth, time.wDay, time.wHour,
             time.wMinute, time.wSecond, time.wMilliseconds, kNames[static_cast<int>(level)]);

    std::lock_guard lock(shared.mutex);
    WriteToFile(line + Utf8(buffer) + "\r\n");
    if (!notice && level != LogLevel::Warning && level != LogLevel::Error)
        return;
    auto& notices = shared.notices;
    // Retries, such as a capture that keeps failing, would otherwise fill the list with one message.
    if (std::any_of(notices.begin(), notices.end(), [&](const Notice& existing) { return existing.text == buffer; }))
        return;
    if (notices.size() == kMaxNotices)
        notices.erase(notices.begin());
    notices.push_back({ level, buffer });
    ++noticeVersion;
}
} // namespace

void InitLog()
{
    path = ExeDirectory() + L"Unishade.log";
    MoveFileExW(path.c_str(), (ExeDirectory() + L"Unishade.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    // GetVersionEx reports Windows 8 to programs without a compatibility manifest.
    RTL_OSVERSIONINFOW version{ sizeof(version) };
    using RtlGetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&version);
    char header[512];
    snprintf(header, sizeof(header), "Unishade %s on Windows %lu.%lu.%lu\r\n", UNISHADE_VERSION,
             version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    WriteToFile(header + std::string("Folder: ") + Utf8(ExeDirectory().c_str()) + "\r\n\r\n");

    if (file == INVALID_HANDLE_VALUE)
        Log(LogLevel::Warning, L"Could not create %ls (error %lu), so nothing is logged this time.", path.c_str(), GetLastError());
}

void Log(LogLevel level, const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, false, format, args);
    va_end(args);
}

void Report(LogLevel level, const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, true, format, args);
    va_end(args);
}

std::vector<Notice> Notices()
{
    std::lock_guard lock(shared.mutex);
    return shared.notices;
}

unsigned NoticeVersion()
{
    return noticeVersion;
}

const std::wstring& LogPath()
{
    return path;
}
