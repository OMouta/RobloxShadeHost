#pragma once

#include <string>

enum class LogLevel
{
    Info,
    Ok,
    Warning,
    Error,
};

// Opens RobloxShadeHost.log beside the exe and writes a header with the version and system. The previous
// run's log is kept as RobloxShadeHost.old.log.
void InitLog();

// Prints a timestamped line to the console and the log file. printf-style; use %ls for wide strings and
// %hs for narrow ones.
void Log(LogLevel level, const wchar_t* format, ...);

const std::wstring& LogPath();
