#pragma once

#include <Windows.h>

#include <string_view>

bool InstallLuaBridge(HMODULE client, std::wstring_view loaderDirectory);
void NotifyTramResourceScriptSeen(std::string_view path);
