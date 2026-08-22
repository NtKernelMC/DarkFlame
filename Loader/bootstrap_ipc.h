#pragma once

#include "config.h"
#include "../Shared/bootstrap_protocol.h"

#include <Windows.h>

#include <string>

class BootstrapIpc
{
public:
    BootstrapIpc(const std::wstring& logDirectory,
        const std::wstring& agentPath, const std::wstring& clientPath,
        const DarkFlameConfig& config)
    {
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64());
        const std::wstring readyName = L"Local\\DarkFlameAgentReady-" + suffix;
        const std::wstring loadedName = L"Local\\DarkFlameClientLoaded-" + suffix;
        const std::wstring publicSerial(config.publicSerial.begin(),
            config.publicSerial.end());

        m_data.magic = BootstrapProtocol::DataMagic;
        m_data.version = BootstrapProtocol::DataVersion;
        m_data.antiShadow = config.antiShadow;
        m_data.setSerial = config.setSerial;
        m_data.randomSerial = config.randomSerial;
        if(!Copy(m_data.logDirectory, logDirectory)
            || !Copy(m_data.agentPath, agentPath)
            || !Copy(m_data.clientPath, clientPath)
            || !Copy(m_data.agentReadyEvent, readyName)
            || !Copy(m_data.clientLoadedEvent, loadedName)
            || !Copy(m_data.publicSerial, publicSerial))
        {
            m_error = ERROR_BUFFER_OVERFLOW;
            return;
        }

        m_ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        if(!m_ready)
        {
            m_error = GetLastError();
            return;
        }
        m_loaded = CreateEventW(nullptr, TRUE, FALSE, loadedName.c_str());
        if(!m_loaded)
        {
            m_error = GetLastError();
            return;
        }
        m_valid = true;
    }

    ~BootstrapIpc()
    {
        if(m_ready)
            CloseHandle(m_ready);
        if(m_loaded)
            CloseHandle(m_loaded);
    }

    BootstrapIpc(const BootstrapIpc&) = delete;
    BootstrapIpc& operator=(const BootstrapIpc&) = delete;

    bool Valid() const { return m_valid; }
    DWORD Error() const { return m_error; }
    HANDLE ReadyEvent() const { return m_ready; }
    HANDLE ClientLoadedEvent() const { return m_loaded; }
    const BootstrapProtocol::Data& Payload() const { return m_data; }

private:
    template<std::size_t Size>
    static bool Copy(wchar_t (&destination)[Size], const std::wstring& source)
    {
        if(source.size() >= Size)
            return false;
        return wcscpy_s(destination, source.c_str()) == 0;
    }

    BootstrapProtocol::Data m_data{};
    HANDLE m_ready{};
    HANDLE m_loaded{};
    DWORD m_error{};
    bool m_valid{};
};
