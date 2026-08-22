#include "memory_module_dumper.h"

#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct PeImage
{
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
};

struct DumpWork
{
    HMODULE module{};
    std::wstring path;
    DWORD delayMs{};
};

std::atomic_bool g_scheduled{};

bool CopyReadable(void* output, const void* input, std::size_t size)
{
    __try
    {
        std::memcpy(output, input, size);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadMemory(std::uintptr_t address, void* output, std::size_t size)
{
    if(!address || !output || !size)
        return false;
    return CopyReadable(output, reinterpret_cast<const void*>(address), size);
}

template<class T>
bool ReadMemory(std::uintptr_t address, T& value)
{
    return ReadMemory(address, &value, sizeof(value));
}

std::uint32_t Align(std::uint32_t value, std::uint32_t alignment)
{
    const std::uint64_t aligned = (static_cast<std::uint64_t>(value) + alignment - 1)
        & ~(static_cast<std::uint64_t>(alignment) - 1);
    return aligned <= UINT32_MAX ? static_cast<std::uint32_t>(aligned) : 0;
}

bool ValidAlignment(std::uint32_t value)
{
    return value >= 0x200 && value <= 0x10000 && !(value & (value - 1));
}

bool Inspect(HMODULE module, PeImage& image)
{
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if(!ReadMemory(base, image.dos) || image.dos.e_magic != IMAGE_DOS_SIGNATURE
        || image.dos.e_lfanew < sizeof(IMAGE_DOS_HEADER))
        return false;
    if(!ReadMemory(base + image.dos.e_lfanew, image.nt)
        || image.nt.Signature != IMAGE_NT_SIGNATURE
        || image.nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || !image.nt.OptionalHeader.SizeOfImage
        || image.nt.OptionalHeader.SizeOfImage > 0x40000000
        || !image.nt.FileHeader.NumberOfSections
        || image.nt.FileHeader.NumberOfSections > 96)
        return false;

    const std::uint64_t sectionOffset = static_cast<std::uint64_t>(image.dos.e_lfanew)
        + offsetof(IMAGE_NT_HEADERS32, OptionalHeader)
        + image.nt.FileHeader.SizeOfOptionalHeader;
    const std::uint64_t sectionBytes = static_cast<std::uint64_t>(
        image.nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if(sectionOffset + sectionBytes > image.nt.OptionalHeader.SizeOfImage)
        return false;

    image.sections.resize(image.nt.FileHeader.NumberOfSections);
    return ReadMemory(base + static_cast<std::uintptr_t>(sectionOffset),
        image.sections.data(), static_cast<std::size_t>(sectionBytes));
}

void CopyImageRange(std::vector<std::uint8_t>& output, std::uint32_t outputOffset,
    std::uintptr_t input, std::uint32_t size)
{
    std::uint32_t copied{};
    while(copied < size)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(reinterpret_cast<const void*>(input + copied),
            &memory, sizeof(memory)))
            break;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress)
            + memory.RegionSize;
        const auto available = static_cast<std::uint32_t>(std::min<std::uintptr_t>(
            size - copied, regionEnd - (input + copied)));
        const DWORD blocked = PAGE_NOACCESS | PAGE_GUARD;
        if(memory.State == MEM_COMMIT && !(memory.Protect & blocked))
            CopyReadable(output.data() + outputOffset + copied,
                reinterpret_cast<const void*>(input + copied), available);
        copied += available;
    }
}

bool BuildDump(HMODULE module, std::vector<std::uint8_t>& output)
{
    PeImage image;
    if(!Inspect(module, image))
        return false;

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const std::uint32_t imageSize = image.nt.OptionalHeader.SizeOfImage;
    const std::uint32_t fileAlignment = ValidAlignment(image.nt.OptionalHeader.FileAlignment)
        ? image.nt.OptionalHeader.FileAlignment : 0x200;
    const std::uint32_t sectionTableEnd = static_cast<std::uint32_t>(image.dos.e_lfanew)
        + offsetof(IMAGE_NT_HEADERS32, OptionalHeader)
        + image.nt.FileHeader.SizeOfOptionalHeader
        + static_cast<std::uint32_t>(image.sections.size() * sizeof(IMAGE_SECTION_HEADER));
    const std::uint32_t headerBytes = std::max<std::uint32_t>(
        image.nt.OptionalHeader.SizeOfHeaders, sectionTableEnd);
    std::uint32_t fileSize = Align(headerBytes, fileAlignment);
    if(!fileSize || headerBytes > imageSize)
        return false;

    struct SectionDump
    {
        std::uint32_t inputOffset{};
        std::uint32_t outputOffset{};
        std::uint32_t copySize{};
        std::uint32_t rawSize{};
    };
    std::vector<SectionDump> dumps(image.sections.size());

    for(std::size_t index = 0; index < image.sections.size(); ++index)
    {
        const IMAGE_SECTION_HEADER& section = image.sections[index];
        if(section.VirtualAddress >= imageSize)
            continue;
        const std::uint32_t mapped = std::max<std::uint32_t>(
            section.Misc.VirtualSize, section.SizeOfRawData);
        const std::uint32_t copySize = std::min<std::uint32_t>(
            mapped, imageSize - section.VirtualAddress);
        const std::uint32_t rawSize = Align(copySize, fileAlignment);
        if(!copySize || !rawSize || fileSize > UINT32_MAX - rawSize)
            continue;
        dumps[index] = {section.VirtualAddress, fileSize, copySize, rawSize};
        fileSize += rawSize;
    }
    if(fileSize > imageSize + fileAlignment * image.sections.size())
        return false;

    output.assign(fileSize, 0);
    CopyImageRange(output, 0, base, headerBytes);
    for(const SectionDump& dump : dumps)
    {
        if(dump.copySize)
            CopyImageRange(output, dump.outputOffset, base + dump.inputOffset, dump.copySize);
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(output.data() + image.dos.e_lfanew);
    auto* sections = reinterpret_cast<IMAGE_SECTION_HEADER*>(
        reinterpret_cast<std::uint8_t*>(&nt->OptionalHeader)
        + nt->FileHeader.SizeOfOptionalHeader);
    nt->OptionalHeader.ImageBase = static_cast<DWORD>(base);
    nt->OptionalHeader.FileAlignment = fileAlignment;
    nt->OptionalHeader.SizeOfHeaders = Align(headerBytes, fileAlignment);
    nt->OptionalHeader.CheckSum = 0;
    if(nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_SECURITY)
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = {};
    for(std::size_t index = 0; index < dumps.size(); ++index)
    {
        sections[index].PointerToRawData = dumps[index].outputOffset;
        sections[index].SizeOfRawData = dumps[index].rawSize;
    }
    return true;
}

bool WriteDump(const std::wstring& path, const std::vector<std::uint8_t>& dump)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
        return false;
    std::size_t offset{};
    bool success = true;
    while(offset < dump.size())
    {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            dump.size() - offset, 16u * 1024u * 1024u));
        DWORD written{};
        if(!WriteFile(file, dump.data() + offset, requested, &written, nullptr)
            || written != requested)
        {
            success = false;
            break;
        }
        offset += written;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    if(!success)
        DeleteFileW(path.c_str());
    return success;
}

DWORD WINAPI DumpThread(void* parameter)
{
    std::unique_ptr<DumpWork> work(static_cast<DumpWork*>(parameter));
    Sleep(work->delayMs);
    std::vector<std::uint8_t> dump;
    const bool built = BuildDump(work->module, dump);
    const bool written = built && WriteDump(work->path, dump);
    if(written)
    {
        std::wstring message = L"[dump] client.dll saved: ";
        message.append(work->path);
        Log::Write(message);
    }
    else
        Log::Write(L"[dump] client.dll memory dump failed");
    g_scheduled.store(false, std::memory_order_release);
    FreeLibrary(work->module);
    return written ? 0 : 1;
}
}

bool MemoryModuleDumper::Schedule(HMODULE module, std::wstring_view path, DWORD delayMs)
{
    if(!module || path.empty() || g_scheduled.exchange(true))
        return false;
    HMODULE held{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &held))
    {
        g_scheduled.store(false);
        return false;
    }

    auto work = std::make_unique<DumpWork>();
    work->module = held;
    work->path = path;
    work->delayMs = delayMs;
    HANDLE thread = CreateThread(nullptr, 0, &DumpThread, work.get(), 0, nullptr);
    if(!thread)
    {
        FreeLibrary(held);
        g_scheduled.store(false);
        return false;
    }
    work.release();
    CloseHandle(thread);
    return true;
}
