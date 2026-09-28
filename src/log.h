#pragma once

#include <string>
#include <vector>

enum class LogLevel
{
    Info,
    Ok,
    Warning,
    Error,
};

struct Notice
{
    LogLevel level;
    std::wstring text;
};

// Opens RobloxShadeHost.log beside the exe and writes a header with the version and system. The previous
// run's log is kept as RobloxShadeHost.old.log.
void InitLog();

// Writes a timestamped line to the log file. Warnings and errors are also shown in the launcher and the
// menu. printf-style; use %ls for wide strings and %hs for narrow ones. Safe from any thread.
void Log(LogLevel level, const wchar_t* format, ...);

// Like Log, but shown in the launcher and the menu at any level. For what the user should see at a glance,
// such as the installation check.
void Report(LogLevel level, const wchar_t* format, ...);

// What Report and warnings and errors have shown so far, oldest first. A repeated message is shown once.
std::vector<Notice> Notices();

// Changes whenever a notice is added.
unsigned NoticeVersion();

const std::wstring& LogPath();
