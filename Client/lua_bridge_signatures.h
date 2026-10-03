#pragma once

#include <string_view>

namespace LuaBridgeSignatures
{
// IsNameAllowed is a separate callee of OnPreFunction, not resolved via the access pattern.
inline constexpr std::string_view CallHookPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 81 EC ? ? ? ? A1 ? ? ? ? "
    "33 C5 89 45 ? 53 56 57 50 8D 45 ? 64 A3 ? ? ? ? 80 3D";
inline constexpr std::string_view LuaNewThreadPattern =
    "55 8B EC 56 8B 75 ? 8B 4E ? 8B 41 ? 3B 41 ? 72 ? 56 E8 ? ? ? ? "
    "83 C4 ? 56";
inline constexpr std::string_view LuaFunctionRegistryPattern =
    "55 8B EC 83 EC ? 56 8B 75 ? 3B 35";
// GetVirtualMachine завиртуализирована; сигнатура указывает на её 5-байтовый jmp-переходник (ILT).
inline constexpr std::string_view GetVirtualMachinePattern =
    "E9 20 E8 55 00";
inline constexpr std::string_view LuaManagerLoadPattern =
    "8B 0D ? ? ? ? 57 C7 45 ? ? ? ? ? E8 ? ? ? ? 85 C0 0F 84 ? ? ? ? "
    "83 78 ? ?";
// AddDebugHook/RemoveDebugHook завиртуализированы; сигнатуры — их точки входа (ABI тот же).
inline constexpr std::string_view AddDebugHookPattern =
    "9C E8 ? ? ? ? B8 AC B4 15 68";
inline constexpr std::string_view RemoveDebugHookPattern =
    "E8 ? ? ? ? 01 94 14 42 55 52 9F";
inline constexpr std::string_view LuaMToRefPattern =
    "55 8B EC 83 EC ? A1 ? ? ? ? 56 8B 75";
inline constexpr std::string_view LuaFunctionRefDtorPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 56 A1 ? ? ? ? 33 C5 50 "
    "8D 45 ? 64 A3 ? ? ? ? 8B F1 FF 76 ? FF 76";
// +1 g_pClientGame, +11 mgr 0xD8, +16 rel32 OnPreFunction; esi=fn, edi=lua_State*; false=skip native
inline constexpr std::string_view ClientGameDebugHookAccessPattern =
    "A1 ? ? ? ? 6A 01 57 56 8B 88 D8 00 00 00 E8 ? ? ? ?";
inline constexpr std::size_t ClientGameSlotOffset = 1;
inline constexpr std::size_t DebugHookManagerFieldOffset = 11;
inline constexpr std::size_t OnPreFunctionCallOffset = 16;
}
