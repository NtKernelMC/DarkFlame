#pragma once

#include "bootstrap_protocol.h"
#include "environment.h"

#include <Windows.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace RuntimeLog
{
inline SRWLOCK g_lock = SRWLOCK_INIT;
inline std::wstring g_path;

inline std::wstring Path()
{
    const std::wstring directory = Environment::Read(BootstrapProtocol::LogDirectoryVariable);
    if (!directory.empty())
        return directory + L"\\DarkFlame.log";

    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size())
    {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);
    const auto separator = path.find_last_of(L"\\/");
    return (separator == std::wstring::npos ? L"." : path.substr(0, separator))
        + L"\\DarkFlame.log";
}

inline void BeginSession()
{
    AcquireSRWLockExclusive(&g_lock);
    g_path = Path();

    const HANDLE file = CreateFileW(g_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char marker[160]{};
        const int size = sprintf_s(marker,
            "\r\n[%04u-%02u-%02u %02u:%02u:%02u.%03u] "
            "========== DarkFlame session P%08lX ==========\r\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        DWORD written{};
        if(size > 0)
            WriteFile(file, marker, static_cast<DWORD>(size), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }

    ReleaseSRWLockExclusive(&g_lock);
}

inline void Write(std::wstring_view text)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[32]{};
    swprintf_s(timestamp, L"[%02u:%02u:%02u.%03u] ",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

    std::wstring line(timestamp);
    line.append(text);
    line.append(L"\r\n");
    const int size = WideCharToMultiByte(CP_UTF8, 0, line.data(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return;

    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
        utf8.data(), size, nullptr, nullptr);

    AcquireSRWLockExclusive(&g_lock);
    if (g_path.empty())
        g_path = Path();
    HANDLE file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written{};
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
}
