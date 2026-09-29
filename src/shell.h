#pragma once

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <thread>

// Help is given on the Unishade Discord server.
inline constexpr wchar_t kHelpUrl[] = L"https://discord.gg/wVbVUdENas";

// Opens a file, folder or web page the way Explorer would. Runs on its own thread, since the host's thread is in
// the multithreaded apartment and shell extensions expect a single-threaded one.
inline void ShellOpen(std::wstring target)
{
    std::thread([target = std::move(target)] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }).detach();
}

// Moves a file to the Recycle Bin. Returns false when it could not.
inline bool Recycle(const std::wstring& path)
{
    bool recycled = false;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring from = path + L'\0';
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        recycled = SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
        CoUninitialize();
    }).join();
    return recycled;
}
