#pragma once

#include "../Shared/bootstrap_protocol.h"
#include "../Shared/environment.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace ModuleUtils
{
inline std::wstring Path(HMODULE module)
{
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size())
    {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);
    return path;
}

inline std::wstring Directory(HMODULE module)
{
    std::wstring path = Path(module);
    if (path.empty() && module)
        path = Environment::Read(BootstrapProtocol::ClientPathVariable);
    if (path.empty() && module)
        path = Path(nullptr);

    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

inline bool HasFileName(std::wstring_view path, std::wstring_view expected)
{
    const auto separator = path.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos
        ? path : path.substr(separator + 1);
    if (name.size() != expected.size())
        return false;
    return std::equal(name.begin(), name.end(), expected.begin(),
        [](wchar_t left, wchar_t right)
        {
            return std::towlower(left) == std::towlower(right);
        });
}
}
