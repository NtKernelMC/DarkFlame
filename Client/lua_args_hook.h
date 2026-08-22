#pragma once

#include <Windows.h>

#include <string_view>

bool StartLuaArgsHook(HMODULE client, std::wstring_view outputDirectory);
bool RepairLuaArgsHook(HMODULE client);
void ResetLuaArgsHook(HMODULE client);
bool IsLuaArgsHookWorker(HMODULE client);
