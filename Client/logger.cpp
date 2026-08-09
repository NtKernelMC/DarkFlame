#include "logger.h"
#include "../Shared/runtime_log.h"

void Log::Initialize(HMODULE)
{
}

void Log::Write(std::wstring_view text)
{
    RuntimeLog::Write(text);
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
