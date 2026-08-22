#pragma once

#include "../Shared/bootstrap_protocol.h"
#include "../Shared/environment.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace ModuleUtils
{
inline bool IsExecutableProtection(DWORD protection)
{
    protection &= 0xFF;
    return protection == PAGE_EXECUTE
        || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE
        || protection == PAGE_EXECUTE_WRITECOPY;
}

inline bool IsExecutableImage(HMODULE module)
{
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if(!base || (base & 3))
        return false;

    MEMORY_BASIC_INFORMATION allocation{};
    if(!VirtualQuery(reinterpret_cast<const void*>(base), &allocation,
        sizeof(allocation)) || allocation.State != MEM_COMMIT
        || allocation.Type != MEM_IMAGE
        || reinterpret_cast<std::uintptr_t>(allocation.AllocationBase) != base)
    {
        return false;
    }

    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + static_cast<std::uintptr_t>(dos->e_lfanew));
        if(nt->Signature != IMAGE_NT_SIGNATURE
            || !nt->OptionalHeader.SizeOfImage)
        {
            return false;
        }

        const auto executablePage = [base](std::uintptr_t rva)
        {
            MEMORY_BASIC_INFORMATION memory{};
            return rva && VirtualQuery(reinterpret_cast<const void*>(base + rva),
                &memory, sizeof(memory)) && memory.State == MEM_COMMIT
                && memory.Type == MEM_IMAGE
                && reinterpret_cast<std::uintptr_t>(memory.AllocationBase) == base
                && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
                && IsExecutableProtection(memory.Protect);
        };
        if(executablePage(nt->OptionalHeader.AddressOfEntryPoint))
            return true;

        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for(WORD index = 0; index < nt->FileHeader.NumberOfSections;
            ++index, ++section)
        {
            if((section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                && executablePage(section->VirtualAddress))
            {
                return true;
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

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
