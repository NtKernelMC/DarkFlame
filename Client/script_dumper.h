#pragma once

#include <Windows.h>

#include <string_view>

namespace Netc
{
class BitStream;
}

bool ConfigureScriptDumper(bool enabled, std::wstring_view loaderDirectory);
bool StartScriptDumperWorkers();
bool IsScriptDumperEnabled();
void ObserveIncomingResourcePacket(unsigned char packetId,
    Netc::BitStream* bitStream);
