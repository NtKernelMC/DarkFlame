#include "lua_args_hook.h"

#include "logger.h"
#include "lua_bridge.h"
#include "netc_hooks.h"
#include "text_display_hook.h"

#include <atomic>
#include <string>

namespace
{
constexpr DWORD RetryDelayMs = 10;

std::atomic<HMODULE> g_clientModule{};
std::atomic<HMODULE> g_workerModule{};
std::atomic_bool g_workerRunning{};
std::atomic<DWORD> g_workerThread{};
std::wstring g_loaderDirectory;

DWORD WINAPI ScanThread(void* parameter)
{
    const HMODULE client = static_cast<HMODULE>(parameter);
    g_workerThread.store(GetCurrentThreadId(), std::memory_order_release);
    bool ready{};
    while(g_clientModule.load(std::memory_order_acquire) == client)
    {
        if(InstallLuaBridge(client, g_loaderDirectory))
        {
            ready = true;
            break;
        }
        Sleep(RetryDelayMs);
    }
    if(ready && g_clientModule.load(std::memory_order_acquire) == client)
    {
        if(!InstallTextDisplayHook(client))
            Log::Write(L"[hook] TextDisplaySetCaption hook unavailable");
        Log::Write(L"[lua-hook] native compiler bridge ready");
    }
    FreeLibrary(client);
    g_workerThread.store(0, std::memory_order_release);
    g_workerModule.store(nullptr, std::memory_order_release);
    g_workerRunning.store(false, std::memory_order_release);
    return 0;
}

bool StartWorker(HMODULE client)
{
    if(g_workerRunning.exchange(true, std::memory_order_acq_rel))
        return true;
    g_workerModule.store(client, std::memory_order_release);
    HMODULE held{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(client), &held))
    {
        g_workerModule.store(nullptr, std::memory_order_release);
        g_workerRunning.store(false, std::memory_order_release);
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, &ScanThread, held, 0, nullptr);
    if(!thread)
    {
        FreeLibrary(held);
        g_workerModule.store(nullptr, std::memory_order_release);
        g_workerRunning.store(false, std::memory_order_release);
        return false;
    }
    CloseHandle(thread);
    Log::Write(L"[lua-hook] native compiler bridge scan started; retrying every 10 ms");
    return true;
}
}

bool StartLuaArgsHook(HMODULE client, std::wstring_view outputDirectory)
{
    if(!client)
        return false;
    const HMODULE previous = g_clientModule.exchange(client,
        std::memory_order_acq_rel);
    if(previous && previous != client)
    {
        ResetLuaBridgeHooks(previous);
        ResetTextDisplayHook(previous);
        ResetNetcClientApi(previous);
    }
    if(!outputDirectory.empty())
        g_loaderDirectory = outputDirectory;
    if(previous == client && RepairLuaArgsHook(client))
        return true;
    return StartWorker(client);
}

bool RepairLuaArgsHook(HMODULE client)
{
    if(!client || g_clientModule.load(std::memory_order_acquire) != client)
        return false;
    if(!RepairLuaBridgeHooks(client))
        return g_workerRunning.load(std::memory_order_acquire);
    return InstallTextDisplayHook(client);
}

void ResetLuaArgsHook(HMODULE client)
{
    if(!client || g_clientModule.load(std::memory_order_acquire) != client)
        return;
    g_clientModule.store(nullptr, std::memory_order_release);
    ResetLuaBridgeHooks(client);
    ResetTextDisplayHook(client);
    ResetNetcClientApi(client);
}

bool IsLuaArgsHookWorker(HMODULE client)
{
    return client && g_workerModule.load(std::memory_order_acquire) == client
        && g_workerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
}
