#pragma once

#include <Windows.h>

#include <string_view>

bool StartForensicHooks(HMODULE client, std::wstring_view outputDirectory);
