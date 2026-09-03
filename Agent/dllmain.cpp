#include "injection.h"
#include "process_utils.h"
#include "../Shared/bootstrap_protocol.h"
#include "../Shared/crash_handler.h"
#include "../Shared/environment.h"
#include "../Shared/runtime_log.h"

#include <Windows.h>
#include <MinHook.h>

#include <string>

namespace
{
    using CreateProcessWFn = decltype(&CreateProcessW);

    std::wstring g_clientPath;
    CreateProcessWFn g_createProcessW{};
    HANDLE g_clientLoadedEvent{};

    bool ApplyBootstrap(const void* reserved)
    {
        if(!reserved)
            return false;
        const auto data = *static_cast<const BootstrapProtocol::Data*>(reserved);
        if(data.magic != BootstrapProtocol::DataMagic
            || data.version != BootstrapProtocol::DataVersion)
        {
            return false;
        }

        return SetEnvironmentVariableW(BootstrapProtocol::LogDirectoryVariable,
                data.logDirectory)
            && SetEnvironmentVariableW(BootstrapProtocol::AgentPathVariable,
                data.agentPath)
            && SetEnvironmentVariableW(BootstrapProtocol::ClientPathVariable,
                data.clientPath)
            && SetEnvironmentVariableW(BootstrapProtocol::AgentReadyEventVariable,
                data.agentReadyEvent)
            && SetEnvironmentVariableW(BootstrapProtocol::ClientLoadedEventVariable,
                data.clientLoadedEvent)
            && SetEnvironmentVariableW(BootstrapProtocol::AntiShadowVariable,
                data.antiShadow ? L"1" : L"0")
            && SetEnvironmentVariableW(BootstrapProtocol::ScriptsDumperVariable,
                data.scriptsDumper ? L"1" : L"0")
            && SetEnvironmentVariableW(BootstrapProtocol::SetSerialVariable,
                data.setSerial ? L"1" : L"0")
            && SetEnvironmentVariableW(BootstrapProtocol::RandomSerialVariable,
                data.randomSerial ? L"1" : L"0")
            && SetEnvironmentVariableW(BootstrapProtocol::PublicSerialVariable,
                data.publicSerial);
    }

    void ClearBootstrapEnvironment()
    {
        Environment::Clear(BootstrapProtocol::LogDirectoryVariable);
        Environment::Clear(BootstrapProtocol::AgentPathVariable);
        Environment::Clear(BootstrapProtocol::ClientPathVariable);
        Environment::Clear(BootstrapProtocol::AgentReadyEventVariable);
        Environment::Clear(BootstrapProtocol::ClientLoadedEventVariable);
        Environment::Clear(BootstrapProtocol::AntiShadowVariable);
        Environment::Clear(BootstrapProtocol::ScriptsDumperVariable);
        Environment::Clear(BootstrapProtocol::SetSerialVariable);
        Environment::Clear(BootstrapProtocol::RandomSerialVariable);
        Environment::Clear(BootstrapProtocol::PublicSerialVariable);
    }

    void CloseClientLoadedEvent()
    {
        if (!g_clientLoadedEvent)
            return;
        CloseHandle(g_clientLoadedEvent);
        g_clientLoadedEvent = nullptr;
    }

    BOOL WINAPI HookCreateProcessW(LPCWSTR applicationName, LPWSTR commandLine,
        LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes,
        BOOL inheritHandles, DWORD creationFlags, LPVOID environment,
        LPCWSTR currentDirectory, LPSTARTUPINFOW startupInfo,
        LPPROCESS_INFORMATION processInformation)
    {
        const bool target = ProcessUtils::IsGameChild(applicationName, commandLine);
        const DWORD flags = target ? creationFlags | CREATE_SUSPENDED : creationFlags;
        const BOOL created = g_createProcessW(applicationName, commandLine,
            processAttributes, threadAttributes, inheritHandles, flags,
            environment, currentDirectory, startupInfo, processInformation);
        if (!target)
            return created;
        if (!created || !processInformation)
        {
            const DWORD error = GetLastError();
            RuntimeLog::Write(L"[agent] gta_sa.exe creation failed");
            ClearBootstrapEnvironment();
            CloseClientLoadedEvent();
            SetLastError(error);
            return created;
        }

        bool exceptionSupport{};
        const bool mapped = MapLibrary(processInformation->hProcess, g_clientPath,
            &exceptionSupport);
        const DWORD mapError = GetLastError();
        ClearBootstrapEnvironment();
        if (!mapped)
        {
            RuntimeLog::Write(L"[agent] DarkFlameClient map failed");
            const DWORD error = mapError ? mapError : ERROR_DLL_INIT_FAILED;
            TerminateProcess(processInformation->hProcess, error);
            WaitForSingleObject(processInformation->hProcess, 5000);
            CloseHandle(processInformation->hThread);
            CloseHandle(processInformation->hProcess);
            *processInformation = {};
            CloseClientLoadedEvent();
            SetLastError(error);
            return FALSE;
        }

        if (g_clientLoadedEvent)
            SetEvent(g_clientLoadedEvent);
        RuntimeLog::Write(L"[agent] DarkFlameClient mapped before gta_sa.exe resume");
        RuntimeLog::Write(exceptionSupport
            ? L"[agent] manual-map exception support registered without PEB entry"
            : L"[agent] manual-map exception support unavailable");
        CloseClientLoadedEvent();
        if (!(creationFlags & CREATE_SUSPENDED))
            ResumeThread(processInformation->hThread);
        return created;
    }

    bool InstallCreateProcessHook()
    {
        const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        void* target = kernelBase
            ? reinterpret_cast<void*>(GetProcAddress(kernelBase, "CreateProcessW")) : nullptr;
        if (!target && kernel32)
            target = reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateProcessW"));
        if (!target)
            return false;

        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookCreateProcessW),
            reinterpret_cast<void**>(&g_createProcessW)) != MH_OK)
            return false;
        if (MH_EnableHook(target) == MH_OK)
            return true;

        MH_RemoveHook(target);
        g_createProcessW = nullptr;
        return false;
    }

    DWORD WINAPI BootstrapThread(void*)
    {
        RuntimeLog::Write(L"[agent] bootstrap started");
        g_clientPath = Environment::Read(BootstrapProtocol::ClientPathVariable);
        HANDLE readyEvent = ProcessUtils::OpenSignalEvent(
            BootstrapProtocol::AgentReadyEventVariable);
        g_clientLoadedEvent = ProcessUtils::OpenSignalEvent(
            BootstrapProtocol::ClientLoadedEventVariable);

        const MH_STATUS status = MH_Initialize();
        const bool initialized = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
        const bool ready = initialized && !g_clientPath.empty() && readyEvent && g_clientLoadedEvent
            && InstallCreateProcessHook();
        if (!ready)
        {
            RuntimeLog::Write(L"[agent] bootstrap failed");
            ClearBootstrapEnvironment();
            CloseClientLoadedEvent();
        }
        if (ready && readyEvent)
            SetEvent(readyEvent);
        if (ready)
            RuntimeLog::Write(L"[agent] CreateProcessW hook ready");
        if (readyEvent)
            CloseHandle(readyEvent);
        return ready ? 0 : 1;
    }
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void* reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if(!ApplyBootstrap(reserved))
            return FALSE;
        RuntimeLog::Write(L"[agent] DllMain attached");
        const std::wstring agentPath = Environment::Read(BootstrapProtocol::AgentPathVariable);
        const std::wstring logDirectory = Environment::Read(
            BootstrapProtocol::LogDirectoryVariable);
        if (!CrashHandler::Install(module, agentPath, L"DarkFlameAgent",
            logDirectory))
        {
            RuntimeLog::Write(L"[agent] crash handler initialization failed");
            ClearBootstrapEnvironment();
            return FALSE;
        }
        RuntimeLog::Write(L"[agent] crash handler ready");
        HANDLE thread = CreateThread(nullptr, 0, &BootstrapThread, nullptr, 0, nullptr);
        if (!thread)
        {
            ClearBootstrapEnvironment();
            CrashHandler::Shutdown();
            return FALSE;
        }
        CloseHandle(thread);
    }
    return TRUE;
}
