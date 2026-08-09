#pragma once

#include <Windows.h>

#include <string>

namespace Environment
{
inline std::wstring Read(const wchar_t* name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed)
        return {};

    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), needed);
    if (!length || length >= needed)
        return {};
    value.resize(length);
    return value;
}

inline void Clear(const wchar_t* name)
{
    if (name && *name)
        SetEnvironmentVariableW(name, nullptr);
}
}
