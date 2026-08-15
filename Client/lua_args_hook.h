#pragma once

#include <Windows.h>

#include <string_view>

bool StartLuaArgsHook(HMODULE client, std::wstring_view outputDirectory);
