#pragma once

#include <Windows.h>
#include <TlHelp32.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ManualMap
{
namespace Detail
{
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using DllMainFn = BOOL(WINAPI*)(HMODULE, DWORD, LPVOID);
using InsertInvertedFunctionTableFn = void(__fastcall*)(void*, ULONG);
using RemoveInvertedFunctionTableFn = void(__fastcall*)(void*);

struct LoaderData
{
    BYTE* image;
    IMAGE_NT_HEADERS32* ntHeaders;
    IMAGE_BASE_RELOCATION* relocations;
    IMAGE_IMPORT_DESCRIPTOR* imports;
    LoadLibraryAFn loadLibrary;
    GetProcAddressFn getProcAddress;
    InsertInvertedFunctionTableFn insertInvertedFunctionTable;
    RemoveInvertedFunctionTableFn removeInvertedFunctionTable;
    ULONG imageSize;
    void* reserved;
};

#pragma optimize("", off)
#pragma code_seg(push, ".mmap$a")
__declspec(noinline) DWORD WINAPI LoadImage(void* parameter)
{
    auto* data = static_cast<LoaderData*>(parameter);
    const DWORD delta = reinterpret_cast<DWORD>(data->image)
        - data->ntHeaders->OptionalHeader.ImageBase;

    if (delta && data->relocations)
    {
        auto* relocation = data->relocations;
        while (relocation->VirtualAddress && relocation->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION))
        {
            const DWORD count = (relocation->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            auto* entries = reinterpret_cast<WORD*>(relocation + 1);
            for (DWORD i = 0; i < count; ++i)
            {
                const WORD type = entries[i] >> 12;
                if (type != IMAGE_REL_BASED_HIGHLOW)
                    continue;

                auto* address = reinterpret_cast<DWORD*>(data->image
                    + relocation->VirtualAddress + (entries[i] & 0x0fff));
                *address += delta;
            }
            relocation = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                reinterpret_cast<BYTE*>(relocation) + relocation->SizeOfBlock);
        }
    }

    if (data->imports)
    {
        for (auto* descriptor = data->imports; descriptor->Name; ++descriptor)
        {
            HMODULE module = data->loadLibrary(
                reinterpret_cast<const char*>(data->image + descriptor->Name));
            if (!module)
                return FALSE;

            const DWORD lookupRva = descriptor->OriginalFirstThunk
                ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
            auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA32*>(data->image + lookupRva);
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(data->image + descriptor->FirstThunk);
            while (lookup->u1.AddressOfData)
            {
                FARPROC function{};
                if (IMAGE_SNAP_BY_ORDINAL32(lookup->u1.Ordinal))
                {
                    function = data->getProcAddress(module,
                        reinterpret_cast<LPCSTR>(IMAGE_ORDINAL32(lookup->u1.Ordinal)));
                }
                else
                {
                    auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        data->image + lookup->u1.AddressOfData);
                    function = data->getProcAddress(module,
                        reinterpret_cast<const char*>(import->Name));
                }
                if (!function)
                    return FALSE;

                thunk->u1.Function = reinterpret_cast<DWORD>(function);
                ++lookup;
                ++thunk;
            }
        }
    }

    const bool registered = data->insertInvertedFunctionTable
        && data->removeInvertedFunctionTable && data->imageSize;
    if (registered)
        data->insertInvertedFunctionTable(data->image, data->imageSize);

    const DWORD entryRva = data->ntHeaders->OptionalHeader.AddressOfEntryPoint;
    if (!entryRva)
        return TRUE;
    const auto entry = reinterpret_cast<DllMainFn>(data->image + entryRva);
    const BOOL attached = entry(reinterpret_cast<HMODULE>(data->image), DLL_PROCESS_ATTACH,
        data->reserved);
    if (!attached && registered)
        data->removeInvertedFunctionTable(data->image);
    return attached;
}
#pragma code_seg(pop)

#pragma code_seg(push, ".mmap$z")
__declspec(noinline) void LoadImageEnd()
{
}
#pragma code_seg(pop)
#pragma optimize("", on)

inline bool InRange(std::size_t offset, std::size_t size, std::size_t total)
{
    return offset <= total && size <= total - offset;
}

inline bool ReadImage(const std::wstring& path, std::vector<BYTE>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > MAXDWORD)
    {
        const DWORD error = GetLastError() ? GetLastError() : ERROR_BAD_LENGTH;
        CloseHandle(file);
        SetLastError(error);
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    const bool success = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
        && read == bytes.size();
    const DWORD error = success ? ERROR_SUCCESS
        : (GetLastError() ? GetLastError() : ERROR_READ_FAULT);
    CloseHandle(file);
    SetLastError(error);
    return success;
}

inline BYTE* FindUniquePattern(HMODULE module, const BYTE* pattern,
    const char* mask, std::size_t patternSize)
{
    if(!module || !pattern || !mask || !patternSize)
        return nullptr;
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        base + dos->e_lfanew);
    if(nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    BYTE* found{};
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for(WORD sectionIndex{}; sectionIndex < nt->FileHeader.NumberOfSections;
        ++sectionIndex)
    {
        const auto& section = sections[sectionIndex];
        if(!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        const std::size_t size = section.Misc.VirtualSize;
        if(size < patternSize || section.VirtualAddress > nt->OptionalHeader.SizeOfImage
            || size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress)
        {
            continue;
        }
        BYTE* start = base + section.VirtualAddress;
        for(std::size_t offset{}; offset <= size - patternSize; ++offset)
        {
            bool matches = true;
            for(std::size_t index{}; index < patternSize; ++index)
            {
                if(mask[index] == 'x' && start[offset + index] != pattern[index])
                {
                    matches = false;
                    break;
                }
            }
            if(!matches)
                continue;
            if(found)
                return nullptr;
            found = start + offset;
        }
    }
    return found;
}

inline BYTE* RemoteModuleBase(HANDLE process, const wchar_t* moduleName)
{
    if(!process || !moduleName)
        return nullptr;
    const DWORD processId = GetProcessId(process);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE
        | TH32CS_SNAPMODULE32, processId);
    if(snapshot != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W module{sizeof(module)};
        if(Module32FirstW(snapshot, &module))
        {
            do
            {
                if(!_wcsicmp(module.szModule, moduleName))
                {
                    CloseHandle(snapshot);
                    return module.modBaseAddr;
                }
            } while(Module32NextW(snapshot, &module));
        }
        CloseHandle(snapshot);
    }

    HMODULE local = GetModuleHandleW(moduleName);
    IMAGE_DOS_HEADER remoteDos{};
    SIZE_T read{};
    if(local && ReadProcessMemory(process, local, &remoteDos,
        sizeof(remoteDos), &read) && read == sizeof(remoteDos)
        && remoteDos.e_magic == IMAGE_DOS_SIGNATURE)
    {
        const auto* localBase = reinterpret_cast<const BYTE*>(local);
        const auto* localDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(localBase);
        const auto* localNt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            localBase + localDos->e_lfanew);
        IMAGE_NT_HEADERS32 remoteNt{};
        if(remoteDos.e_lfanew > 0 && ReadProcessMemory(process,
            reinterpret_cast<const BYTE*>(local) + remoteDos.e_lfanew,
            &remoteNt, sizeof(remoteNt), &read) && read == sizeof(remoteNt)
            && remoteNt.Signature == IMAGE_NT_SIGNATURE
            && localNt->Signature == IMAGE_NT_SIGNATURE
            && remoteNt.FileHeader.TimeDateStamp == localNt->FileHeader.TimeDateStamp
            && remoteNt.OptionalHeader.SizeOfImage
                == localNt->OptionalHeader.SizeOfImage)
        {
            return reinterpret_cast<BYTE*>(local);
        }
    }
    return nullptr;
}

inline void ResolveInvertedFunctionTable(HANDLE process,
    InsertInvertedFunctionTableFn& insert,
    RemoveInvertedFunctionTableFn& remove)
{
    insert = nullptr;
    remove = nullptr;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    BYTE* remoteNtdll = RemoteModuleBase(process, L"ntdll.dll");
    if(!ntdll || !remoteNtdll)
        return;

    constexpr BYTE insertPattern[]{0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x83,
        0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8D, 0x45, 0xF8, 0x8B, 0xFA,
        0x50, 0x8D, 0x55, 0xFC, 0x8B, 0xD9, 0xE8};
    constexpr BYTE removePattern[]{0x8B, 0xFF, 0x56, 0x68, 0, 0, 0, 0,
        0x8B, 0xF1, 0xE8, 0, 0, 0, 0, 0x8B, 0xD6, 0xE8, 0, 0, 0, 0,
        0x5E, 0xE9};
    BYTE* localInsert = FindUniquePattern(ntdll, insertPattern,
        "xxxxxxxxxxxxxxxxxxxxxxx", sizeof(insertPattern));
    BYTE* localRemove = FindUniquePattern(ntdll, removePattern,
        "xxxx????xxx????xxx????xx", sizeof(removePattern));
    if(!localInsert || !localRemove)
        return;

    BYTE* localBase = reinterpret_cast<BYTE*>(ntdll);
    insert = reinterpret_cast<InsertInvertedFunctionTableFn>(remoteNtdll
        + (localInsert - localBase));
    remove = reinterpret_cast<RemoveInvertedFunctionTableFn>(remoteNtdll
        + (localRemove - localBase));
}
}

inline bool Map(HANDLE process, const std::wstring& path,
    const void* parameter = nullptr, SIZE_T parameterSize = 0,
    bool* exceptionSupport = nullptr)
{
    if(exceptionSupport)
        *exceptionSupport = false;
    if (!process || path.empty() || (!parameter && parameterSize)
        || (parameter && !parameterSize))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    std::vector<BYTE> file;
    if (!Detail::ReadImage(path, file))
        return false;
    if (!Detail::InRange(0, sizeof(IMAGE_DOS_HEADER), file.size()))
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0
        || !Detail::InRange(static_cast<std::size_t>(dos->e_lfanew),
            sizeof(IMAGE_NT_HEADERS32), file.size()))
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)
        || !nt->OptionalHeader.SizeOfImage
        || !Detail::InRange(0, nt->OptionalHeader.SizeOfHeaders, file.size()))
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }

    const std::size_t sectionsOffset = static_cast<std::size_t>(dos->e_lfanew)
        + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t sectionsSize = static_cast<std::size_t>(nt->FileHeader.NumberOfSections)
        * sizeof(IMAGE_SECTION_HEADER);
    if (!Detail::InRange(sectionsOffset, sectionsSize, file.size()))
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }

    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file.data() + sectionsOffset);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (sections[i].SizeOfRawData
            && (!Detail::InRange(sections[i].PointerToRawData, sections[i].SizeOfRawData, file.size())
                || sections[i].VirtualAddress > nt->OptionalHeader.SizeOfImage
                || sections[i].SizeOfRawData > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress))
        {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return false;
        }
    }

    void* remoteImage = VirtualAllocEx(process,
        reinterpret_cast<void*>(nt->OptionalHeader.ImageBase), nt->OptionalHeader.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteImage)
    {
        remoteImage = VirtualAllocEx(process, nullptr, nt->OptionalHeader.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!remoteImage)
        return false;

    const auto fail = [&](DWORD error)
    {
        VirtualFreeEx(process, remoteImage, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    };

    const auto& relocDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (reinterpret_cast<std::uintptr_t>(remoteImage) != nt->OptionalHeader.ImageBase
        && !relocDirectory.VirtualAddress)
    {
        return fail(ERROR_INVALID_ADDRESS);
    }

    if (!WriteProcessMemory(process, remoteImage, file.data(), nt->OptionalHeader.SizeOfHeaders, nullptr))
        return fail(GetLastError());
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (!sections[i].SizeOfRawData)
            continue;
        void* destination = static_cast<BYTE*>(remoteImage) + sections[i].VirtualAddress;
        const void* source = file.data() + sections[i].PointerToRawData;
        if (!WriteProcessMemory(process, destination, source, sections[i].SizeOfRawData, nullptr))
            return fail(GetLastError());
    }

    const auto& importDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void* remoteParameter{};
    if (parameterSize)
    {
        remoteParameter = VirtualAllocEx(process, nullptr, parameterSize,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteParameter)
            return fail(GetLastError());
        if (!WriteProcessMemory(process, remoteParameter, parameter,
            parameterSize, nullptr))
        {
            const DWORD error = GetLastError();
            VirtualFreeEx(process, remoteParameter, 0, MEM_RELEASE);
            return fail(error);
        }
    }

    Detail::LoaderData data{};
    data.image = static_cast<BYTE*>(remoteImage);
    data.ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(
        data.image + static_cast<std::size_t>(dos->e_lfanew));
    data.relocations = relocDirectory.VirtualAddress
        ? reinterpret_cast<IMAGE_BASE_RELOCATION*>(data.image + relocDirectory.VirtualAddress) : nullptr;
    data.imports = importDirectory.VirtualAddress
        ? reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(data.image + importDirectory.VirtualAddress) : nullptr;
    data.loadLibrary = &LoadLibraryA;
    data.getProcAddress = &GetProcAddress;
    Detail::ResolveInvertedFunctionTable(process,
        data.insertInvertedFunctionTable, data.removeInvertedFunctionTable);
    data.imageSize = nt->OptionalHeader.SizeOfImage;
    data.reserved = remoteParameter;
    if(exceptionSupport)
    {
        *exceptionSupport = data.insertInvertedFunctionTable
            && data.removeInvertedFunctionTable;
    }

    const auto loaderStart = reinterpret_cast<const BYTE*>(&Detail::LoadImage);
    const auto loaderEnd = reinterpret_cast<const BYTE*>(&Detail::LoadImageEnd);
    if (loaderEnd <= loaderStart)
        return fail(ERROR_INVALID_FUNCTION);
    const SIZE_T loaderSize = static_cast<SIZE_T>(loaderEnd - loaderStart);
    const SIZE_T allocationSize = sizeof(data) + loaderSize;
    void* remoteLoader = VirtualAllocEx(process, nullptr, allocationSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteLoader)
    {
        const DWORD error = GetLastError();
        if (remoteParameter)
            VirtualFreeEx(process, remoteParameter, 0, MEM_RELEASE);
        return fail(error);
    }

    auto cleanup = [&](DWORD error, bool releaseImage)
    {
        VirtualFreeEx(process, remoteLoader, 0, MEM_RELEASE);
        if (remoteParameter)
            VirtualFreeEx(process, remoteParameter, 0, MEM_RELEASE);
        if (releaseImage)
            VirtualFreeEx(process, remoteImage, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    };

    if (!WriteProcessMemory(process, remoteLoader, &data, sizeof(data), nullptr)
        || !WriteProcessMemory(process, static_cast<BYTE*>(remoteLoader) + sizeof(data),
            loaderStart, loaderSize, nullptr))
    {
        return cleanup(GetLastError(), true);
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<BYTE*>(remoteLoader) + sizeof(data)),
        remoteLoader, 0, nullptr);
    if (!thread)
        return cleanup(GetLastError(), true);

    const DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode{};
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exitCode);
    const DWORD error = completed && exitCode ? ERROR_SUCCESS
        : (wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_DLL_INIT_FAILED);
    CloseHandle(thread);
    if (!completed || !exitCode)
    {
        if (wait == WAIT_TIMEOUT)
        {
            SetLastError(error);
            return false;
        }
        return cleanup(error, true);
    }

    VirtualFreeEx(process, remoteLoader, 0, MEM_RELEASE);
    if (remoteParameter)
        VirtualFreeEx(process, remoteParameter, 0, MEM_RELEASE);
    SetLastError(ERROR_SUCCESS);
    return true;
}
}
