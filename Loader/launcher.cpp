#include "launcher.h"

#include "bootstrap_ipc.h"
#include "path_utils.h"
#include "registry.h"
#include "../Shared/manual_map.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace
{
constexpr wchar_t AgentName[] = L"DarkFlameAgent.dll";
constexpr wchar_t ClientName[] = L"DarkFlameClient.dll";
constexpr wchar_t GameExe[] = L"Multi Theft Auto.exe";

struct ProcessHandles
{
    PROCESS_INFORMATION value{};

    ~ProcessHandles()
    {
        if(value.hThread)
            CloseHandle(value.hThread);
        if(value.hProcess)
            CloseHandle(value.hProcess);
    }
};

std::wstring ErrorText(std::wstring_view prefix, DWORD error)
{
    return std::wstring(prefix) + std::to_wstring(error);
}
}

int Launcher::Run(const DarkFlameConfig& config, std::stop_token stop,
    const LogSink& log)
{
    log(L"[info] searching for MTA Province...");
    const auto installDirectory = InstallRegistry::FindInstallDirectory();
    if(!installDirectory)
    {
        log(L"[error] installation path was not found in the registry");
        return 1;
    }

    const std::wstring ownDirectory = LoaderPath::OwnDirectory();
    const std::wstring executable = *installDirectory + L"\\" + GameExe;
    const std::wstring agent = ownDirectory + L"\\" + AgentName;
    const std::wstring client = ownDirectory + L"\\" + ClientName;
    if(!LoaderPath::FileExists(executable) || !LoaderPath::FileExists(agent)
        || !LoaderPath::FileExists(client))
    {
        log(L"[error] required file is missing");
        if(!LoaderPath::FileExists(executable))
            log(L"[error] game: " + executable);
        if(!LoaderPath::FileExists(agent))
            log(L"[error] agent: " + agent);
        if(!LoaderPath::FileExists(client))
            log(L"[error] client: " + client);
        return 2;
    }

    log(config.antiShadow ? L"[config] Anti-Shadow enabled"
        : L"[config] Anti-Shadow disabled");
    log(config.setSerial ? L"[config] Black Mirror enabled"
        : L"[config] Black Mirror disabled");
    log(config.randomSerial ? L"[config] Random Serial enabled"
        : L"[config] Random Serial disabled");

    BootstrapIpc ipc(ownDirectory, agent, client, config);
    if(!ipc.Valid())
    {
        ipc.ClearEnvironment();
        log(ErrorText(L"[error] bootstrap setup failed: ", ipc.Error()));
        return 3;
    }
    log(L"[info] bootstrap environment ready");
    if(stop.stop_requested())
        return 0;

    std::wstring commandLine = L"\"" + executable + L"\" upd";
    std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
    command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    ProcessHandles process;
    log(L"[info] starting: " + executable);
    if(!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, installDirectory->c_str(), &startup,
        &process.value))
    {
        const DWORD error = GetLastError();
        ipc.ClearEnvironment();
        log(ErrorText(L"[error] CreateProcessW failed: ", error));
        return 3;
    }
    ipc.ClearEnvironment();
    log(L"[info] MTA launcher created suspended");

    bool injected = ManualMap::Map(process.value.hProcess, agent);
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
        TerminateProcess(process.value.hProcess, injectionError);
        WaitForSingleObject(process.value.hProcess, 5000);
        return 4;
    }

    log(L"[ok] DarkFlameAgent.dll manually mapped and ready");
    ResumeThread(process.value.hThread);
    log(L"[info] waiting for DarkFlameClient.dll mapping...");
    while(!stop.stop_requested())
    {
        const DWORD wait = WaitForSingleObject(ipc.ClientLoadedEvent(), 50);
        if(wait == WAIT_OBJECT_0)
        {
            log(L"[ok] DarkFlameClient.dll mapping reported successful");
            return 0;
        }
        if(wait == WAIT_FAILED)
        {
            log(ErrorText(L"[error] client event wait failed: ", GetLastError()));
            return 5;
        }
    }
    log(L"[info] loader closed while the game continues starting");
    return 0;
}
