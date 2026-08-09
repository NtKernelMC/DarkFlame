#pragma once

#include "../Shared/environment.h"

#include <Windows.h>

#include <cwctype>
#include <string>

namespace ProcessUtils
{
inline bool IsGameChild(const wchar_t* applicationName, const wchar_t* commandLine)
{
    std::wstring candidate;
    if (applicationName)
        candidate.append(applicationName);
    candidate.push_back(L' ');
    if (commandLine)
        candidate.append(commandLine);
    for (wchar_t& character : candidate)
        character = static_cast<wchar_t>(std::towlower(character));
    return candidate.find(L"gta_sa") != std::wstring::npos
        || candidate.find(L"proxy_sa") != std::wstring::npos;
}

inline HANDLE OpenSignalEvent(const wchar_t* variable)
{
    const std::wstring name = Environment::Read(variable);
    return name.empty() ? nullptr : OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
}
}
