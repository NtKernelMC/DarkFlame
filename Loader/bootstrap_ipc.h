#pragma once

#include "../Shared/bootstrap_protocol.h"

#include <Windows.h>

#include <string>

class BootstrapIpc
{
public:
    BootstrapIpc(const std::wstring& logDirectory, const std::wstring& agentPath,
        const std::wstring& clientPath)
    {
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64());
        const std::wstring readyName = L"Local\\DarkFlameAgentReady-" + suffix;
        const std::wstring loadedName = L"Local\\DarkFlameClientLoaded-" + suffix;

        m_ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        if (!m_ready)
        {
            m_error = GetLastError();
            return;
        }
        m_loaded = CreateEventW(nullptr, TRUE, FALSE, loadedName.c_str());
        if (!m_loaded)
        {
            m_error = GetLastError();
            return;
        }
        if (!SetEnvironmentVariableW(BootstrapProtocol::LogDirectoryVariable, logDirectory.c_str())
            || !SetEnvironmentVariableW(BootstrapProtocol::AgentPathVariable, agentPath.c_str())
            || !SetEnvironmentVariableW(BootstrapProtocol::ClientPathVariable, clientPath.c_str())
            || !SetEnvironmentVariableW(BootstrapProtocol::AgentReadyEventVariable, readyName.c_str())
            || !SetEnvironmentVariableW(BootstrapProtocol::ClientLoadedEventVariable, loadedName.c_str()))
        {
            m_error = GetLastError();
            return;
        }
        m_valid = true;
    }

    ~BootstrapIpc()
    {
        ClearEnvironment();
        if (m_ready)
            CloseHandle(m_ready);
        if (m_loaded)
            CloseHandle(m_loaded);
    }

    BootstrapIpc(const BootstrapIpc&) = delete;
    BootstrapIpc& operator=(const BootstrapIpc&) = delete;

    bool Valid() const { return m_valid; }
    DWORD Error() const { return m_error; }
    HANDLE ReadyEvent() const { return m_ready; }
    HANDLE ClientLoadedEvent() const { return m_loaded; }
    void ClearEnvironment()
    {
        SetEnvironmentVariableW(BootstrapProtocol::LogDirectoryVariable, nullptr);
        SetEnvironmentVariableW(BootstrapProtocol::AgentPathVariable, nullptr);
        SetEnvironmentVariableW(BootstrapProtocol::ClientPathVariable, nullptr);
        SetEnvironmentVariableW(BootstrapProtocol::AgentReadyEventVariable, nullptr);
        SetEnvironmentVariableW(BootstrapProtocol::ClientLoadedEventVariable, nullptr);
    }

private:
    HANDLE m_ready{};
    HANDLE m_loaded{};
    DWORD m_error{};
    bool m_valid{};
};
