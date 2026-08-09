#include "logger.h"
#include "module_utils.h"
#include "netc_hooks.h"
#include "../Shared/bootstrap_protocol.h"
#include "../Shared/crash_handler.h"
#include "../Shared/environment.h"

#include <Windows.h>
#include <MinHook.h>
#include <winternl.h>

#include <atomic>
#include <string_view>

namespace
{
using LdrLoadDllFn = NTSTATUS(NTAPI*)(PWSTR, ULONG, PUNICODE_STRING, PHANDLE);

constexpr NTSTATUS DllInitFailed = static_cast<NTSTATUS>(0xC0000142L);

HMODULE g_module{};
void* g_ldrLoadDllTarget{};
LdrLoadDllFn g_ldrLoadDll{};
std::atomic_bool g_clientLogged{};

bool IsLoadedModule(const UNICODE_STRING* moduleName, std::wstring_view expected)
{
    if (!moduleName || !moduleName->Buffer || !moduleName->Length)
        return false;
    const std::wstring_view name(moduleName->Buffer, moduleName->Length / sizeof(wchar_t));
    return ModuleUtils::HasFileName(name, expected);
}

NTSTATUS NTAPI HookLdrLoadDll(PWSTR searchPath, ULONG flags,
    PUNICODE_STRING moduleName, PHANDLE moduleHandle)
{
    const NTSTATUS status = g_ldrLoadDll(searchPath, flags, moduleName, moduleHandle);
    if (status < 0)
        return status;

    if (IsLoadedModule(moduleName, L"client.dll") && !g_clientLogged.exchange(true))
        Log::Write(L"[loader] client.dll found");

    if (IsLoadedModule(moduleName, L"netc.dll"))
    {
        const HMODULE netc = moduleHandle ? static_cast<HMODULE>(*moduleHandle) : nullptr;
        if (!InstallNetcHooks(netc))
        {
            Log::Write(L"[bootstrap] netc hooks failed; rejecting module load");
            return DllInitFailed;
        }
        Log::Write(L"[bootstrap] netc hooks ready before LdrLoadDll returned");
    }
    return status;
}

bool InstallLoaderHook()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ldrLoadDllTarget = ntdll
        ? reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrLoadDll")) : nullptr;
    if (!g_ldrLoadDllTarget)
        return false;

    const MH_STATUS created = MH_CreateHook(g_ldrLoadDllTarget,
        reinterpret_cast<void*>(&HookLdrLoadDll), reinterpret_cast<void**>(&g_ldrLoadDll));
    if (created != MH_OK)
        return false;
    if (MH_EnableHook(g_ldrLoadDllTarget) == MH_OK)
        return true;

    MH_RemoveHook(g_ldrLoadDllTarget);
    g_ldrLoadDllTarget = nullptr;
    g_ldrLoadDll = nullptr;
    return false;
}

void RemoveLoaderHook()
{
    if (!g_ldrLoadDllTarget)
        return;
    MH_DisableHook(g_ldrLoadDllTarget);
    MH_RemoveHook(g_ldrLoadDllTarget);
    g_ldrLoadDllTarget = nullptr;
    g_ldrLoadDll = nullptr;
}

bool InitializeRuntime()
{
    Log::Initialize(g_module);

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log::Write(L"[bootstrap] MinHook initialization failed");
        return false;
    }
    if (!InstallLoaderHook())
    {
        Log::Write(L"[bootstrap] LdrLoadDll hook failed");
        MH_Uninitialize();
        return false;
    }

    if (GetModuleHandleW(L"client.dll") && !g_clientLogged.exchange(true))
        Log::Write(L"[loader] client.dll found");

    const HMODULE netc = GetModuleHandleW(L"netc.dll");
    if (netc && !InstallNetcHooks(netc))
    {
        Log::Write(L"[bootstrap] preloaded netc hook installation failed");
        RemoveLoaderHook();
        MH_Uninitialize();
        return false;
    }

    Log::Write(L"[bootstrap] DarkFlameClient ready before gta_sa.exe resume");
    return true;
}
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    g_module = module;
    DisableThreadLibraryCalls(module);
    Log::Initialize(module);
    Log::Write(L"[client] DllMain attached");
    const std::wstring clientPath = Environment::Read(BootstrapProtocol::ClientPathVariable);
    Environment::Clear(BootstrapProtocol::LogDirectoryVariable);
    Environment::Clear(BootstrapProtocol::AgentPathVariable);
    Environment::Clear(BootstrapProtocol::ClientPathVariable);
    Environment::Clear(BootstrapProtocol::AgentReadyEventVariable);
    Environment::Clear(BootstrapProtocol::ClientLoadedEventVariable);
    if (!CrashHandler::Install(module, clientPath, L"DarkFlameClient"))
    {
        Log::Write(L"[client] crash handler initialization failed");
        return FALSE;
    }
    if (InitializeRuntime())
        return TRUE;
    Log::Write(L"[client] runtime initialization failed");
    CrashHandler::Shutdown();
    return FALSE;
}
