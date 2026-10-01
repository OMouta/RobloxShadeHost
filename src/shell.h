#pragma once

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <functional>
#include <memory>
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

enum class RecycleResult
{
    Pending,
    Recycled,
    Cancelled,
    Failed,
};

// Moves a file to the Recycle Bin on a thread of its own, so the caller keeps drawing and checks the result later.
// Where the file cannot go to the Recycle Bin, such as on a drive without one, Windows asks before deleting it for
// good. done runs on that thread once it is over.
inline std::shared_ptr<std::atomic<RecycleResult>> Recycle(std::wstring path, std::function<void()> done = {})
{
    auto result = std::make_shared<std::atomic<RecycleResult>>(RecycleResult::Pending);
    std::thread([path = std::move(path), done = std::move(done), result] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        RecycleResult outcome = RecycleResult::Failed;
        IFileOperation* operation = nullptr;
        IShellItem* item = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation))) &&
            SUCCEEDED(operation->SetOperationFlags(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT)) &&
            SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            SUCCEEDED(operation->DeleteItem(item, nullptr)))
        {
            const HRESULT performed = operation->PerformOperations();
            BOOL aborted = FALSE;
            if (SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted)
                outcome = RecycleResult::Cancelled;
            else if (SUCCEEDED(performed))
                outcome = RecycleResult::Recycled;
        }
        if (item)
            item->Release();
        if (operation)
            operation->Release();
        if (SUCCEEDED(com))
            CoUninitialize();
        result->store(outcome);
        if (done)
            done();
    }).detach();
    return result;
}
