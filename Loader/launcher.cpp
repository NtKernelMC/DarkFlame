#include "launcher.h"

#include "bootstrap_ipc.h"
#include "config.h"
#include "path_utils.h"
#include "../Shared/manual_map.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <string>

namespace
{
constexpr wchar_t AgentName[] = L"DarkFlameAgent.dll";
constexpr wchar_t ClientName[] = L"DarkFlameClient.dll";
constexpr wchar_t GameExe[] = L"Multi Theft Auto.exe";
constexpr DWORD PollIntervalMs = 1;

struct ProcessHandle
{
    HANDLE value{};

    ~ProcessHandle()
    {
        if(value)
            CloseHandle(value);
    }
};

std::wstring ErrorText(std::wstring_view prefix, DWORD error)
{
    return std::wstring(prefix) + std::to_wstring(error);
}

DWORD FindGameProcess()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD processId{};
    if(Process32FirstW(snapshot, &entry))
    {
        do
        {
            if(!_wcsicmp(entry.szExeFile, GameExe))
            {
                processId = entry.th32ProcessID;
                break;
            }
        } while(Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processId;
}

HANDLE OpenGameProcess(DWORD processId)
{
    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION
        | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE;
    return OpenProcess(access, FALSE, processId);
}
}

int Launcher::Run(std::stop_token stop, const LogSink& log)
{
    const std::wstring ownDirectory = LoaderPath::OwnDirectory();
    const std::wstring agent = ownDirectory + L"\\" + AgentName;
    const std::wstring client = ownDirectory + L"\\" + ClientName;
    if(!LoaderPath::FileExists(agent) || !LoaderPath::FileExists(client))
    {
        log(L"[error] required file is missing");
        if(!LoaderPath::FileExists(agent))
            log(L"[error] agent: " + agent);
        if(!LoaderPath::FileExists(client))
            log(L"[error] client: " + client);
        return 2;
    }

    log(L"[info] waiting for Multi Theft Auto.exe (1 ms poll)...");
    ProcessHandle process;
    DWORD processId{};
    while(!stop.stop_requested())
    {
        processId = FindGameProcess();
        if(processId)
            process.value = OpenGameProcess(processId);
        if(process.value)
            break;
        Sleep(PollIntervalMs);
    }
    if(stop.stop_requested())
    {
        log(L"[info] agent loading cancelled");
        return 0;
    }

    log(L"[info] Multi Theft Auto.exe found, PID "
        + std::to_wstring(processId));
    const DarkFlameConfig config = Config::Load(ownDirectory);
    log(config.antiShadow ? L"[config] Anti-Shadow enabled"
        : L"[config] Anti-Shadow disabled");
    log(config.setSerial ? L"[config] Black Mirror enabled"
        : L"[config] Black Mirror disabled");
    log(config.randomSerial ? L"[config] Random Serial enabled"
        : L"[config] Random Serial disabled");

    BootstrapIpc ipc(ownDirectory, agent, client, config);
    if(!ipc.Valid())
    {
        log(ErrorText(L"[error] bootstrap setup failed: ", ipc.Error()));
        return 3;
    }
    const auto& payload = ipc.Payload();
    bool injected = ManualMap::Map(process.value, agent, &payload,
        sizeof(payload));
    DWORD injectionError = GetLastError();
    if(injected)
    {
        const DWORD wait = WaitForSingleObject(ipc.ReadyEvent(), 5000);
        if(wait != WAIT_OBJECT_0)
        {
            injected = false;
            injectionError = wait == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT;
        }
    }
    if(!injected)
    {
        log(ErrorText(L"[error] agent bootstrap failed: ", injectionError));
        return 4;
    }

    log(L"[ok] DarkFlameAgent.dll manually mapped and ready");
    log(L"[info] waiting for DarkFlameClient.dll mapping...");
    const HANDLE waits[]{ipc.ClientLoadedEvent(), process.value};
    while(!stop.stop_requested())
    {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 50);
        if(wait == WAIT_OBJECT_0)
        {
            log(L"[ok] DarkFlameClient.dll mapping reported successful");
            return 0;
        }
        if(wait == WAIT_OBJECT_0 + 1)
        {
            log(L"[error] Multi Theft Auto.exe exited before client mapping");
            return 5;
        }
        if(wait == WAIT_FAILED)
        {
            log(ErrorText(L"[error] client event wait failed: ", GetLastError()));
            return 5;
        }
    }
    log(L"[info] loader closed while MTA continues running");
    return 0;
}
