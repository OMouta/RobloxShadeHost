#include "net.h"
#include "text.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
struct InternetHandleDeleter
{
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetHandleDeleter>;

std::string ErrorText(DWORD error)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE,
                   GetModuleHandleW(L"winhttp.dll"), error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = text ? text : L"error " + std::to_wstring(error);
    LocalFree(text);
    while (!message.empty() && wcschr(L"\r\n .", message.back()))
        message.pop_back();
    return Utf8(message);
}

class Sha256Hasher
{
public:
    Sha256Hasher()
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)))
            throw std::runtime_error("SHA-256 is unavailable on this system.");
    }
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    ~Sha256Hasher()
    {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }

    void Add(const char* data, size_t size)
    {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(size), 0);
    }

    std::string Finish()
    {
        unsigned char digest[32]{};
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        std::string text;
        for (unsigned char byte : digest)
        {
            text += "0123456789abcdef"[byte >> 4];
            text += "0123456789abcdef"[byte & 15];
        }
        return text;
    }

private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

void Get(const std::wstring& url, const std::function<void(const char*, size_t)>& sink, const std::atomic<bool>& cancel,
         const DownloadProgress& progress)
{
    const auto fail = [&](const std::string& reason) { throw std::runtime_error("Could not download " + Utf8(url) + ": " + reason + "."); };

    // A null component with a non-zero length makes WinHttpCrackUrl point into url.
    URL_COMPONENTS parts{ sizeof(parts) };
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        fail("not an HTTPS address");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const std::wstring agent = L"RobloxShadeHost-Setup/" + Wide(ROBLOX_SHADE_HOST_VERSION);
    InternetHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        fail(ErrorText(GetLastError()));
    WinHttpSetTimeouts(session.get(), 30000, 30000, 30000, 30000);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    InternetHandle request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                      : nullptr);
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        fail(ErrorText(GetLastError()));

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
        fail("the server answered " + std::to_string(status));
    uint64_t total = 0;
    size = sizeof(total);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64, WINHTTP_HEADER_NAME_BY_INDEX, &total,
                             &size, WINHTTP_NO_HEADER_INDEX))
        total = 0;

    std::vector<char> buffer(1 << 16);
    uint64_t received = 0;
    for (;;)
    {
        if (cancel)
            throw Cancelled{};
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            fail(ErrorText(GetLastError()));
        if (!read)
            break;
        sink(buffer.data(), read);
        received += read;
        if (progress)
            progress(received, total);
    }
    if (total && received != total)
        fail("the connection closed early");
}
} // namespace

std::string Fetch(const std::wstring& url, const std::atomic<bool>& cancel, const DownloadProgress& progress)
{
    std::string data;
    Get(url, [&](const char* chunk, size_t size) { data.append(chunk, size); }, cancel, progress);
    return data;
}

void Download(const std::wstring& url, const std::filesystem::path& path, const std::string& sha256, const std::atomic<bool>& cancel,
              const DownloadProgress& progress)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("Could not write " + Utf8(path.wstring()) + ".");
    try
    {
        Sha256Hasher hasher;
        Get(url,
            [&](const char* chunk, size_t size) {
                file.write(chunk, size);
                hasher.Add(chunk, size);
            },
            cancel, progress);
        file.close();
        if (!file)
            throw std::runtime_error("Could not write " + Utf8(path.wstring()) + ".");
        if (!sha256.empty() && hasher.Finish() != sha256)
            throw std::runtime_error("The download from " + Utf8(url) + " does not match its checksum.");
    }
    catch (...)
    {
        file.close();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
}
