#include "logger.h"
#include "../Shared/runtime_log.h"

#include <cstdio>
#include <mutex>

namespace
{
SRWLOCK g_tramLogLock = SRWLOCK_INIT;
std::wstring g_tramLogPath;

std::wstring TramLogPath()
{
    std::wstring path = RuntimeLog::Path();
    const std::size_t separator = path.find_last_of(L"\\/");
    path.resize(separator == std::wstring::npos ? 0 : separator + 1);
    path += L"TramBot.log";
    return path;
}

void ClearTramLog()
{
    AcquireSRWLockExclusive(&g_tramLogLock);
    g_tramLogPath = TramLogPath();
    HANDLE file = CreateFileW(g_tramLogPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    ReleaseSRWLockExclusive(&g_tramLogLock);
}
}

void Log::Initialize(HMODULE)
{
    static std::once_flag once;
    std::call_once(once, &ClearTramLog);
}

void Log::Write(std::wstring_view text)
{
    RuntimeLog::Write(text);
}

void Log::Tram(std::string_view text)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char timestamp[32]{};
    sprintf_s(timestamp, "[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds);
    std::string line(timestamp);
    line.append(text);
    line.append("\r\n");

    AcquireSRWLockExclusive(&g_tramLogLock);
    if(g_tramLogPath.empty())
        g_tramLogPath = TramLogPath();
    HANDLE file = CreateFileW(g_tramLogPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file != INVALID_HANDLE_VALUE)
    {
        DWORD written{};
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_tramLogLock);
}

void Log::Scan(std::wstring_view name, std::wstring_view status, std::uintptr_t address)
{
    std::wstring line = L"[scan] ";
    line.append(name);
    line.append(L": ");
    line.append(status);
    if (address)
    {
        wchar_t buffer[24]{};
        swprintf_s(buffer, L" @ 0x%08lX", static_cast<unsigned long>(address));
        line.append(buffer);
    }
    Write(line);
}
