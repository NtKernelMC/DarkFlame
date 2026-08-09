#include "bootstrap_ipc.h"
#include "console.h"
#include "path_utils.h"
#include "registry.h"
#include "../Shared/manual_map.h"
#include "../Shared/runtime_log.h"

#include <Windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t AgentName[] = L"DarkFlameAgent.dll";
    constexpr wchar_t ClientName[] = L"DarkFlameClient.dll";
    constexpr wchar_t GameExe[] = L"Multi Theft Auto.exe";
}

int wmain()
{
    SetConsoleTitleW(L"DarkFlame");
    Console::SetBrightPink();
    RuntimeLog::Clear();
    RuntimeLog::Write(L"[loader] started");
    std::wprintf(L"[info] searching for MTA Province...\n");

    const auto installDirectory = InstallRegistry::FindInstallDirectory();
    if (!installDirectory)
    {
        std::wprintf(L"[error] installation path was not found in the registry\n");
        return Console::WaitForEnter(1);
    }

    const std::wstring ownDirectory = LoaderPath::OwnDirectory();
    const std::wstring executable = *installDirectory + L"\\" + GameExe;
    const std::wstring agent = ownDirectory + L"\\" + AgentName;
    const std::wstring client = ownDirectory + L"\\" + ClientName;
    if (!LoaderPath::FileExists(executable) || !LoaderPath::FileExists(agent)
        || !LoaderPath::FileExists(client))
    {
        std::wprintf(L"[error] missing file:\n  game: %ls\n  agent: %ls\n  client: %ls\n",
            executable.c_str(), agent.c_str(), client.c_str());
        return Console::WaitForEnter(2);
    }

    BootstrapIpc ipc(ownDirectory, agent, client);
    if (!ipc.Valid())
    {
        ipc.ClearEnvironment();
        std::wprintf(L"[error] bootstrap setup failed: %lu\n", ipc.Error());
        return Console::WaitForEnter(3);
    }
    RuntimeLog::Write(L"[loader] bootstrap environment ready");

    std::wstring commandLine = L"\"" + executable + L"\" upd";
    std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
    command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wprintf(L"[info] starting: %ls\n", executable.c_str());
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, installDirectory->c_str(), &startup, &process))
    {
        const DWORD error = GetLastError();
        ipc.ClearEnvironment();
        std::wprintf(L"[error] CreateProcessW failed: %lu\n", error);
        return Console::WaitForEnter(3);
    }
    RuntimeLog::Write(L"[loader] MTA launcher created suspended");
    ipc.ClearEnvironment();

    bool injected = ManualMap::Map(process.hProcess, agent);
    DWORD injectionError = GetLastError();
    if (injected)
    {
        const DWORD wait = WaitForSingleObject(ipc.ReadyEvent(), 5000);
        if (wait != WAIT_OBJECT_0)
        {
            injected = false;
            injectionError = wait == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT;
        }
    }

    if (injected)
    {
        RuntimeLog::Write(L"[loader] DarkFlameAgent mapped and ready");
        std::wprintf(L"[ok] DarkFlameAgent.dll manually mapped and ready\n");
    }
    else
    {
        RuntimeLog::Write(L"[loader] DarkFlameAgent bootstrap failed");
        std::wprintf(L"[error] agent bootstrap failed: %lu\n", injectionError);
    }

    if (injected)
        ResumeThread(process.hThread);
    else
    {
        TerminateProcess(process.hProcess, injectionError);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!injected)
        return Console::WaitForEnter(4);
    if (!Console::WaitForClientOrEnter(ipc.ClientLoadedEvent()))
        return 0;

    std::wprintf(L"[ok] DarkFlameAgent reported successful DarkFlameClient.dll mapping\n");
    RuntimeLog::Write(L"[loader] DarkFlameClient map reported successful");
    std::wprintf(L"[info] closing loader in 3 seconds...\n");
    Sleep(3000);
    return 0;
}
