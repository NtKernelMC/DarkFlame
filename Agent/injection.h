#pragma once

#include <Windows.h>

#include <string>

bool MapLibrary(HANDLE process, const std::wstring& path,
    bool* exceptionSupport = nullptr);
