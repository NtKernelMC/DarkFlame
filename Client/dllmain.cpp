#include "forensic_hooks.h"
#include "gui.h"
#include "logger.h"
#include "lua_args_hook.h"
#include "memory_module_dumper.h"
#include "module_utils.h"
#include "netc_hooks.h"
#include "privacy_hooks.h"
#include "../Shared/bootstrap_protocol.h"
#include "../Shared/crash_handler.h"
#include "../Shared/environment.h"

#include <Windows.h>
#include <MinHook.h>
#include <winternl.h>

#include <atomic>
#include <string>
#include <string_view>

namespace
{
using LdrLoadDllFn = NTSTATUS(NTAPI*)(PWSTR, ULONG, PUNICODE_STRING, PHANDLE);
using LdrUnloadDllFn = NTSTATUS(NTAPI*)(HMODULE);

constexpr NTSTATUS DllInitFailed = static_cast<NTSTATUS>(0xC0000142L);

HMODULE g_module{};
void* g_ldrLoadDllTarget{};
LdrLoadDllFn g_ldrLoadDll{};
void* g_ldrUnloadDllTarget{};
LdrUnloadDllFn g_ldrUnloadDll{};
std::atomic<HMODULE> g_clientModule{};
std::atomic<HMODULE> g_netcModule{};
std::wstring g_loaderDirectory;
bool g_antiShadow{true};
bool g_setSerial{};
bool g_randomSerial{};
std::string g_publicSerial{"9F5A1A5F9008ED9327D64B4A700324F3"};

bool ConfigFlag(const wchar_t* name, bool fallback)
{
    const std::wstring value = Environment::Read(name);
    if(value.empty())
        return fallback;
    return value == L"1";
}

std::string Ascii(std::wstring_view value)
{
    std::string output;
    output.reserve(value.size());
    for(wchar_t ch : value)
    {
        if(ch > 0x7F)
            return {};
        output.push_back(static_cast<char>(ch));
    }
    return output;
}

void ScheduleClientDump(HMODULE client)
{
    if(!client || g_loaderDirectory.empty())
    {
        Log::Write(L"[dump] client.dll dump skipped: loader directory unavailable");
        return;
    }
    const std::wstring path = g_loaderDirectory + L"\\client.unpacked.dll";
    if(MemoryModuleDumper::Schedule(client, path))
        Log::Write(L"[dump] client.dll memory dump scheduled in 5 seconds");
    else
        Log::Write(L"[dump] client.dll memory dump scheduling failed");
}

bool IsLoadedModule(const UNICODE_STRING* moduleName, std::wstring_view expected)
{
    if (!moduleName || !moduleName->Buffer || !moduleName->Length)
        return false;
    const std::wstring_view name(moduleName->Buffer, moduleName->Length / sizeof(wchar_t));
    return ModuleUtils::HasFileName(name, expected);
}

void HandleClientLoaded(HMODULE client)
{
    if(!ModuleUtils::IsExecutableImage(client))
        return;
    const HMODULE previous = g_clientModule.exchange(client,
        std::memory_order_acq_rel);
    if(g_antiShadow)
        UpdatePrivacyClient(client);
    if(previous != client)
    {
        Log::Write(L"[loader] client.dll found");
        ScheduleClientDump(client);
    }
    if(!StartLuaArgsHook(client, g_loaderDirectory))
       Log::Write(L"[lua-hook] client hook startup failed");
}

NTSTATUS NTAPI HookLdrLoadDll(PWSTR searchPath, ULONG flags,
    PUNICODE_STRING moduleName, PHANDLE moduleHandle)
{
    const NTSTATUS status = g_ldrLoadDll(searchPath, flags, moduleName, moduleHandle);
    if (status < 0)
        return status;

    if(IsLoadedModule(moduleName, L"client.dll"))
    {
        const HMODULE client = moduleHandle
            ? static_cast<HMODULE>(*moduleHandle) : GetModuleHandleW(L"client.dll");
        if(ModuleUtils::IsExecutableImage(client))
            HandleClientLoaded(client);
        else
            Log::Write(L"[loader] ignored non-executable client.dll mapping");
    }

    if (IsLoadedModule(moduleName, L"netc.dll"))
    {
        const HMODULE netc = moduleHandle ? static_cast<HMODULE>(*moduleHandle) : nullptr;
        if(!ModuleUtils::IsExecutableImage(netc))
        {
            Log::Write(L"[loader] ignored non-executable netc.dll mapping");
            return status;
        }
        if (!InstallNetcHooks(netc))
        {
            Log::Write(L"[bootstrap] netc hooks failed; rejecting module load");
            return DllInitFailed;
        }
        g_netcModule.store(netc, std::memory_order_release);
        Log::Write(L"[bootstrap] netc hooks ready before LdrLoadDll returned");
    }
    return status;
}

NTSTATUS NTAPI HookLdrUnloadDll(HMODULE module)
{
    const bool trackedClient = module
        && g_clientModule.load(std::memory_order_acquire) == module;
    const bool trackedNetc = module
        && g_netcModule.load(std::memory_order_acquire) == module;
    const NTSTATUS status = g_ldrUnloadDll(module);
    if(status < 0)
        return status;

    if(trackedClient)
    {
        const HMODULE live = GetModuleHandleW(L"client.dll");
        if(live == module && ModuleUtils::IsExecutableImage(live))
        {
            static std::atomic_uint32_t ignored{};
            if(ignored.fetch_add(1, std::memory_order_relaxed) < 4)
                Log::Write(L"[loader] client.dll reference released; executable mapping retained");
        }
        else
        {
            if(g_antiShadow)
                UpdatePrivacyClient(nullptr);
            g_clientModule.store(nullptr, std::memory_order_release);
            Log::Write(L"[loader] executable client.dll mapping removed");
        }
    }
    if(trackedNetc)
    {
        const HMODULE live = GetModuleHandleW(L"netc.dll");
        if(live != module || !ModuleUtils::IsExecutableImage(live))
            g_netcModule.store(nullptr, std::memory_order_release);
    }
    return status;
}

bool InstallLoaderHook()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ldrLoadDllTarget = ntdll
        ? reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrLoadDll")) : nullptr;
    g_ldrUnloadDllTarget = ntdll
        ? reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrUnloadDll")) : nullptr;
    if(!g_ldrLoadDllTarget || !g_ldrUnloadDllTarget)
        return false;

    const MH_STATUS loadCreated = MH_CreateHook(g_ldrLoadDllTarget,
        reinterpret_cast<void*>(&HookLdrLoadDll), reinterpret_cast<void**>(&g_ldrLoadDll));
    if(loadCreated != MH_OK)
        return false;
    const MH_STATUS unloadCreated = MH_CreateHook(g_ldrUnloadDllTarget,
        reinterpret_cast<void*>(&HookLdrUnloadDll),
        reinterpret_cast<void**>(&g_ldrUnloadDll));
    if(unloadCreated != MH_OK)
    {
        MH_RemoveHook(g_ldrLoadDllTarget);
        return false;
    }
    if(MH_QueueEnableHook(g_ldrLoadDllTarget) == MH_OK
        && MH_QueueEnableHook(g_ldrUnloadDllTarget) == MH_OK
        && MH_ApplyQueued() == MH_OK)
    {
        return true;
    }

    MH_RemoveHook(g_ldrLoadDllTarget);
    MH_RemoveHook(g_ldrUnloadDllTarget);
    g_ldrLoadDllTarget = nullptr;
    g_ldrLoadDll = nullptr;
    g_ldrUnloadDllTarget = nullptr;
    g_ldrUnloadDll = nullptr;
    return false;
}

void RemoveLoaderHook()
{
    if(g_ldrLoadDllTarget)
    {
        MH_DisableHook(g_ldrLoadDllTarget);
        MH_RemoveHook(g_ldrLoadDllTarget);
    }
    if(g_ldrUnloadDllTarget)
    {
        MH_DisableHook(g_ldrUnloadDllTarget);
        MH_RemoveHook(g_ldrUnloadDllTarget);
    }
    g_ldrLoadDllTarget = nullptr;
    g_ldrLoadDll = nullptr;
    g_ldrUnloadDllTarget = nullptr;
    g_ldrUnloadDll = nullptr;
}

DWORD WINAPI HookWatchdog(void*)
{
    for(;;)
    {
        HMODULE client = GetModuleHandleW(L"client.dll");
        if(!ModuleUtils::IsExecutableImage(client))
            client = nullptr;
        const HMODULE tracked = g_clientModule.load(std::memory_order_acquire);
        if(client)
        {
            if(client != tracked)
                HandleClientLoaded(client);
            else
            {
                if(g_antiShadow)
                    UpdatePrivacyClient(client);
                if(!RepairLuaArgsHook(client))
                   StartLuaArgsHook(client, g_loaderDirectory);
            }
        }
        else if(tracked)
        {
            if(g_antiShadow)
                UpdatePrivacyClient(nullptr);
            g_clientModule.store(nullptr, std::memory_order_release);
        }

        HMODULE netc = GetModuleHandleW(L"netc.dll");
        if(!ModuleUtils::IsExecutableImage(netc))
            netc = nullptr;
        if(netc)
        {
            g_netcModule.store(netc, std::memory_order_release);
            InstallNetcHooks(netc);
        }
        if(g_antiShadow)
            RepairPrivacyHooks();
        Sleep(1000);
    }
}

bool StartHookWatchdog()
{
    HANDLE thread = CreateThread(nullptr, 0, &HookWatchdog, nullptr, 0, nullptr);
    if(!thread)
        return false;
    CloseHandle(thread);
    return true;
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
    if(!ConfigureNetcHooks(g_setSerial, g_randomSerial, g_publicSerial))
    {
        Log::Write(L"[serial] invalid PUBLIC_SERIAL configuration");
        return false;
    }
    if (!InstallLoaderHook())
    {
        Log::Write(L"[bootstrap] LdrLoadDll hook failed");
        MH_Uninitialize();
        return false;
    }

    if(!StartGui(g_module))
        Log::Write(L"[gui] startup failed");

    if(!StartForensicHooks(GetModuleHandleW(L"client.dll"), g_loaderDirectory))
        Log::Write(L"[process-filter] early hook startup failed");

    const HMODULE client = GetModuleHandleW(L"client.dll");
    if(g_antiShadow)
    {
        if(!StartPrivacyHooks(client))
            Log::Write(L"[privacy] hook startup failed");
    }
    else
    {
        Log::Write(L"[privacy] disabled by ANTI_SHADOW=0");
    }

    const HMODULE netc = GetModuleHandleW(L"netc.dll");
    if (netc && !InstallNetcHooks(netc))
    {
        Log::Write(L"[bootstrap] preloaded netc hook installation failed");
        RemoveLoaderHook();
        MH_Uninitialize();
        return false;
    }

    if(client)
        HandleClientLoaded(client);

    if(!StartHookWatchdog())
        Log::Write(L"[bootstrap] hook watchdog startup failed");

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
    g_loaderDirectory = Environment::Read(BootstrapProtocol::LogDirectoryVariable);
    const std::wstring clientPath = Environment::Read(BootstrapProtocol::ClientPathVariable);
    g_antiShadow = ConfigFlag(BootstrapProtocol::AntiShadowVariable, true);
    g_setSerial = ConfigFlag(BootstrapProtocol::SetSerialVariable, false);
    g_randomSerial = ConfigFlag(BootstrapProtocol::RandomSerialVariable, false);
    const std::wstring publicSerial = Environment::Read(BootstrapProtocol::PublicSerialVariable);
    if(!publicSerial.empty())
        g_publicSerial = Ascii(publicSerial);
    if(g_loaderDirectory.empty())
    {
        const auto separator = clientPath.find_last_of(L"\\/");
        if(separator != std::wstring::npos)
            g_loaderDirectory = clientPath.substr(0, separator);
    }
    Environment::Clear(BootstrapProtocol::LogDirectoryVariable);
    Environment::Clear(BootstrapProtocol::AgentPathVariable);
    Environment::Clear(BootstrapProtocol::ClientPathVariable);
    Environment::Clear(BootstrapProtocol::AgentReadyEventVariable);
    Environment::Clear(BootstrapProtocol::ClientLoadedEventVariable);
    Environment::Clear(BootstrapProtocol::AntiShadowVariable);
    Environment::Clear(BootstrapProtocol::SetSerialVariable);
    Environment::Clear(BootstrapProtocol::RandomSerialVariable);
    Environment::Clear(BootstrapProtocol::PublicSerialVariable);
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
