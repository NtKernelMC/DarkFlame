#pragma once

#include <Windows.h>

#include <optional>
#include <string>

namespace InstallRegistry
{
inline std::optional<std::wstring> ReadString(HKEY root, const wchar_t* key,
    const wchar_t* value, REGSAM view)
{
    HKEY handle{};
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &handle) != ERROR_SUCCESS)
        return std::nullopt;

    DWORD type{};
    DWORD size{};
    const LONG query = RegQueryValueExW(handle, value, nullptr, &type, nullptr, &size);
    if (query != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)
        || size < sizeof(wchar_t))
    {
        RegCloseKey(handle);
        return std::nullopt;
    }

    std::wstring result(size / sizeof(wchar_t), L'\0');
    const LONG read = RegQueryValueExW(handle, value, nullptr, &type,
        reinterpret_cast<BYTE*>(result.data()), &size);
    RegCloseKey(handle);
    if (read != ERROR_SUCCESS)
        return std::nullopt;

    while (!result.empty() && result.back() == L'\0')
        result.pop_back();
    if (type != REG_EXPAND_SZ)
        return result;

    const DWORD needed = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
    if (!needed)
        return result;
    std::wstring expanded(needed, L'\0');
    if (!ExpandEnvironmentStringsW(result.c_str(), expanded.data(), needed))
        return result;
    while (!expanded.empty() && expanded.back() == L'\0')
        expanded.pop_back();
    return expanded;
}

inline std::optional<std::wstring> FindInstallDirectory()
{
    constexpr wchar_t key[] = L"SOFTWARE\\Multi Theft Auto: Province All\\1.6";
    if (auto path = ReadString(HKEY_LOCAL_MACHINE, key,
        L"Last Install Location", KEY_WOW64_32KEY))
    {
        return path;
    }

    constexpr wchar_t legacyKey[] = L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: Province All\\1.6";
    return ReadString(HKEY_LOCAL_MACHINE, legacyKey,
        L"Last Install Location", KEY_WOW64_64KEY);
}
}
