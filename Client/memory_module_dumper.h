#pragma once

#include <Windows.h>

#include <string_view>

namespace MemoryModuleDumper
{
bool Schedule(HMODULE module, std::wstring_view path, DWORD delayMs = 5'000);
}
