#include "../Shared/crash_handler.h"

#include <filesystem>
#include <iostream>

const BYTE* DumpByte(void* dump, DWORD64 address)
{
    MINIDUMP_DIRECTORY* directory{};
    void* stream{};
    ULONG size{};
    const auto* bytes = static_cast<const BYTE*>(dump);
    if(MiniDumpReadDumpStream(dump, MemoryListStream, &directory, &stream, &size))
    {
        const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(stream);
        for(ULONG i = 0; i < list->NumberOfMemoryRanges; ++i)
        {
            const auto& range = list->MemoryRanges[i];
            if(address >= range.StartOfMemoryRange
                && address - range.StartOfMemoryRange < range.Memory.DataSize)
                return bytes + range.Memory.Rva + address - range.StartOfMemoryRange;
        }
    }
    if(MiniDumpReadDumpStream(dump, Memory64ListStream, &directory, &stream, &size))
    {
        const auto* list = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
        DWORD64 offset = list->BaseRva;
        for(ULONG64 i = 0; i < list->NumberOfMemoryRanges; ++i)
        {
            const auto& range = list->MemoryRanges[i];
            if(address >= range.StartOfMemoryRange
                && address - range.StartOfMemoryRange < range.DataSize)
                return bytes + offset + address - range.StartOfMemoryRange;
            offset += range.DataSize;
        }
    }
    return nullptr;
}

int wmain()
{
    constexpr SIZE_T allocationSize = 1024 * 1024;
    auto* allocation = static_cast<BYTE*>(VirtualAlloc(nullptr, allocationSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if(!allocation) return 1;
    for(SIZE_T i = 0; i < allocationSize; ++i)
        allocation[i] = static_cast<BYTE>(37 + i / 4096);

    const auto directory = std::filesystem::current_path()
        / (L"crash-memory-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(directory);
    CrashHandler::g_process = GetCurrentProcess();
    CrashHandler::Copy(CrashHandler::g_artifactDirectory, directory.wstring());
    CrashHandler::Copy(CrashHandler::g_moduleName, L"MemoryTest");
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000001;
    record.ExceptionAddress = reinterpret_cast<void*>(context.Eip);
    EXCEPTION_POINTERS exception{&record, &context};
    CrashHandler::WriteMiniDump(&exception);

    std::filesystem::path path;
    for(const auto& entry : std::filesystem::directory_iterator(directory))
        if(entry.path().extension() == L".dmp") path = entry.path();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE) return 2;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* dump = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if(!dump) return 3;
    const auto* header = static_cast<const MINIDUMP_HEADER*>(dump);
    bool passed = (header->Flags & MiniDumpWithPrivateReadWriteMemory) != 0;
    for(SIZE_T i = 0; i < allocationSize; i += 4096)
    {
        const BYTE* captured = DumpByte(dump, reinterpret_cast<DWORD64>(allocation + i));
        if(!captured || *captured != allocation[i])
        {
            std::cerr << "Missing private memory at offset " << i << '\n';
            passed = false;
            break;
        }
    }
    UnmapViewOfFile(dump);
    CloseHandle(mapping);
    CloseHandle(file);
    VirtualFree(allocation, 0, MEM_RELEASE);
    std::cout << (passed ? "PASS" : "FAIL") << ": private memory capture\n";
    return passed ? 0 : 4;
}
