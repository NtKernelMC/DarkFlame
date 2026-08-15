#pragma once

#include <Windows.h>

#include <string_view>

bool InstallNetcHooks(HMODULE netc);
bool InitializeNetcClientApi(HMODULE client);
void SetNetcLuaCallContext(std::string_view resource);
void ClearNetcLuaCallContext();
