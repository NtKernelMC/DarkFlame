#pragma once

#include <Windows.h>

#include <conio.h>
#include <cstdio>

namespace Console
{
inline void SetBrightPink()
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute(output, FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
}

inline int WaitForEnter(int exitCode)
{
    std::wprintf(L"[info] press Enter to close...\n");
    while (_getwch() != L'\r')
    {
    }
    return exitCode;
}

inline bool WaitForClientOrEnter(HANDLE clientLoadedEvent)
{
    std::wprintf(L"[info] waiting for DarkFlameClient.dll; press Enter to close loader...\n");
    for (;;)
    {
        if (WaitForSingleObject(clientLoadedEvent, 50) == WAIT_OBJECT_0)
            return true;
        if (!_kbhit())
            continue;

        const wchar_t key = _getwch();
        if (key == L'\r')
            return false;
        if (key == 0 || key == 0xe0)
            _getwch();
    }
}
}
