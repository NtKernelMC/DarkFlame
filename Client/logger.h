#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace Log
{
void Initialize(HMODULE module);
void Write(std::wstring_view text);
void Tram(std::string_view text);
void Scan(std::wstring_view name, std::wstring_view status, std::uintptr_t address = 0);
}
