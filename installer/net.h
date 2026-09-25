#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Called with the bytes received so far and the total, which is 0 when the server does not send one.
using DownloadProgress = std::function<void(uint64_t received, uint64_t total)>;

// Thrown when the user cancels.
struct Cancelled
{
};

// Downloads an HTTPS url into memory. Throws std::runtime_error with a readable message, or Cancelled.
std::string Fetch(const std::wstring& url, const std::atomic<bool>& cancel, const DownloadProgress& progress = {});

// Downloads an HTTPS url to a file. A non-empty sha256 (lowercase hex) must match the downloaded data.
void Download(const std::wstring& url, const std::filesystem::path& path, const std::string& sha256, const std::atomic<bool>& cancel,
              const DownloadProgress& progress = {});
