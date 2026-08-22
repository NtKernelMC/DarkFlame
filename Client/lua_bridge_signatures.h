#pragma once

#include <string_view>

namespace LuaBridgeSignatures
{
inline constexpr std::string_view IsNameAllowedPattern =
    "55 8B EC 8B 4D ? B8 ? ? ? ? 53 56 33 DB";
inline constexpr std::string_view CallHookPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 81 EC ? ? ? ? A1 ? ? ? ? "
    "33 C5 89 45 ? 53 56 57 50 8D 45 ? 64 A3 ? ? ? ? 80 3D";
inline constexpr std::string_view LuaNewThreadPattern =
    "55 8B EC 56 8B 75 ? 8B 4E ? 8B 41 ? 3B 41 ? 72 ? 56 E8 ? ? ? ? "
    "83 C4 ? 56";
inline constexpr std::string_view LuaFunctionRegistryPattern =
    "55 8B EC 83 EC ? 56 8B 75 ? 3B 35";
inline constexpr std::string_view GetVirtualMachinePattern =
    "55 8B EC 83 EC ? 53 57 8B 7D ? 8B D9 85 FF";
inline constexpr std::string_view LuaManagerLoadPattern =
    "8B 0D ? ? ? ? 57 C7 45 ? ? ? ? ? E8 ? ? ? ? 85 C0 0F 84 ? ? ? ? "
    "83 78 ? ?";
inline constexpr std::string_view AddDebugHookPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 81 EC ? ? ? ? A1 ? ? ? ? "
    "33 C5 89 45 ? 53 56 57 50 8D 45 ? 64 A3 ? ? ? ? 8B 45 ? 8B 55";
inline constexpr std::string_view RemoveDebugHookPattern =
    "55 8B EC A1 ? ? ? ? 53 56 57 8B B0";
inline constexpr std::string_view LuaMToRefPattern =
    "55 8B EC 83 EC ? A1 ? ? ? ? 56 8B 75";
inline constexpr std::string_view LuaFunctionRefDtorPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 56 A1 ? ? ? ? 33 C5 50 "
    "8D 45 ? 64 A3 ? ? ? ? 8B F1 FF 76 ? FF 76";
inline constexpr std::string_view ClientGameDebugHookAccessPattern =
    "A1 ? ? ? ? 8D 8D ? ? ? ? 51 8D 8D ? ? ? ? 51 FF B5 ? ? ? ? "
    "8B 88 ? ? ? ? E8 ? ? ? ?";
}
