#pragma once

#include <Windows.h>

#include <cstddef>

namespace BootstrapProtocol
{
inline constexpr DWORD DataMagic = 0x46444253;
inline constexpr DWORD DataVersion = 1;
inline constexpr std::size_t PathCapacity = 1024;
inline constexpr std::size_t EventCapacity = 128;
inline constexpr std::size_t SerialCapacity = 64;

struct Data
{
    DWORD magic{};
    DWORD version{};
    wchar_t logDirectory[PathCapacity]{};
    wchar_t agentPath[PathCapacity]{};
    wchar_t clientPath[PathCapacity]{};
    wchar_t agentReadyEvent[EventCapacity]{};
    wchar_t clientLoadedEvent[EventCapacity]{};
    wchar_t publicSerial[SerialCapacity]{};
    DWORD antiShadow{};
    DWORD setSerial{};
    DWORD randomSerial{};
};

inline constexpr wchar_t LogDirectoryVariable[] = L"DARKFLAME_LOG_DIRECTORY";
inline constexpr wchar_t AgentPathVariable[] = L"DARKFLAME_AGENT_PATH";
inline constexpr wchar_t ClientPathVariable[] = L"DARKFLAME_CLIENT_PATH";
inline constexpr wchar_t AgentReadyEventVariable[] = L"DARKFLAME_AGENT_READY_EVENT";
inline constexpr wchar_t ClientLoadedEventVariable[] = L"DARKFLAME_CLIENT_LOADED_EVENT";
inline constexpr wchar_t AntiShadowVariable[] = L"DARKFLAME_ANTI_SHADOW";
inline constexpr wchar_t SetSerialVariable[] = L"DARKFLAME_SET_SERIAL";
inline constexpr wchar_t RandomSerialVariable[] = L"DARKFLAME_RANDOM_SERIAL";
inline constexpr wchar_t PublicSerialVariable[] = L"DARKFLAME_PUBLIC_SERIAL";
}
