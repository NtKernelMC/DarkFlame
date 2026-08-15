#include "lua_args_hook.h"

#include "logger.h"
#include "lua_bridge.h"
#include "signature_scanner.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view VmpGatePattern =
    "E8 ? ? ? ? 57 51 9C BF 8B 06 3A B5 8B CF C1 E9 02 8B 7C 24 08 C7 44";
constexpr DWORD RetryDelayMs = 1;

std::atomic_bool g_started{};
std::atomic_uint32_t g_dumpSequence{};
std::atomic_uint32_t g_gateHits{};
std::uintptr_t g_vmpTarget{};
std::wstring g_dumpDirectory;

bool ReadBytes(std::uintptr_t address, void* output, std::size_t size)
{
    if(!address || !output || !size)
        return false;
    __try
    {
        std::memcpy(output, reinterpret_cast<const void*>(address), size);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Readable(std::uintptr_t address)
{
    MEMORY_BASIC_INFORMATION info{};
    if(address < 0x10000 || !VirtualQuery(reinterpret_cast<const void*>(address),
        &info, sizeof(info)) || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE
        || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool ReadableRange(std::uintptr_t address, std::size_t size)
{
    if(!address || !size || address + size < address)
        return false;
    const std::uintptr_t end = address + size;
    while(address < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))
            || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = info.Protect & 0xFF;
        if(protection != PAGE_READONLY && protection != PAGE_READWRITE
            && protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ
            && protection != PAGE_EXECUTE_READWRITE
            && protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress)
            + static_cast<std::uintptr_t>(info.RegionSize);
        if(regionEnd <= address)
            return false;
        address = std::min(end, regionEnd);
    }
    return true;
}

bool ReadText(std::uintptr_t address, std::string& output,
    std::size_t capacity = 4096)
{
    output.clear();
    if(!Readable(address))
        return false;
    __try
    {
        const auto* text = reinterpret_cast<const char*>(address);
        for(std::size_t index = 0; index < capacity; ++index)
        {
            if(!text[index])
                return !output.empty();
            output.push_back(text[index]);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        output.clear();
    }
    return false;
}

bool ContainsI(std::string_view text, std::string_view needle)
{
    if(needle.empty() || text.size() < needle.size())
        return false;
    for(std::size_t index = 0; index <= text.size() - needle.size(); ++index)
    {
        if(!_strnicmp(text.data() + index, needle.data(), needle.size()))
            return true;
    }
    return false;
}

std::wstring Wide(std::string_view value)
{
    if(value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if(size <= 0)
        return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
        output.data(), size);
    return output;
}

void CleanDumpDirectory()
{
    if(g_dumpDirectory.empty())
        return;
    CreateDirectoryW(g_dumpDirectory.c_str(), nullptr);
    const std::wstring mask = g_dumpDirectory + L"\\*.lua";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(mask.c_str(), &data);
    if(find == INVALID_HANDLE_VALUE)
        return;

    std::uint32_t removed{};
    do
    {
        if(!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            const std::wstring path = g_dumpDirectory + L"\\" + data.cFileName;
            if(DeleteFileW(path.c_str()))
                ++removed;
        }
    }
    while(FindNextFileW(find, &data));
    FindClose(find);
    g_dumpSequence.store(0, std::memory_order_release);
    Log::Write(L"[lua-hook] cleared old ProvinceAC Lua dumps: "
        + std::to_wstring(removed));
}

void DumpProvinceAc(std::uintptr_t buffer, std::size_t size,
    std::uintptr_t pathAddress)
{
    constexpr std::size_t MaxLuaBuffer = 128u * 1024u * 1024u;
    if(g_dumpDirectory.empty() || !size || size > MaxLuaBuffer
        || !ReadableRange(buffer, size))
        return;
    std::string path;
    if(!ReadText(pathAddress, path) || !ContainsI(path, "province_ac"))
        return;
    const std::size_t separator = path.find_last_of("\\/");
    std::string name = separator == std::string::npos
        ? path : path.substr(separator + 1);
    if(name.empty())
        name = "script.luac";
    for(char& character : name)
    {
        if(character == '<' || character == '>' || character == ':'
            || character == '"' || character == '/' || character == '\\'
            || character == '|' || character == '?' || character == '*')
            character = '_';
    }
    CreateDirectoryW(g_dumpDirectory.c_str(), nullptr);
    wchar_t prefix[48]{};
    swprintf_s(prefix, L"%08lX_%06lu_", GetCurrentProcessId(),
        g_dumpSequence.fetch_add(1) + 1);
    const std::wstring outputPath = g_dumpDirectory + L"\\" + prefix + Wide(name);
    HANDLE file = CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
        return;
    DWORD written{};
    const BOOL result = WriteFile(file, reinterpret_cast<const void*>(buffer),
        static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(file);
    wchar_t message[512]{};
    swprintf_s(message, L"[lua-hook] province_ac dump %ls: %ls (%lu/%lu bytes)",
        result && written == size ? L"saved" : L"failed", outputPath.c_str(),
        written, static_cast<unsigned long>(size));
    Log::Write(message);
}

bool LuaHeader(std::uintptr_t address)
{
    unsigned char header[5]{};
    return ReadBytes(address, header, sizeof(header)) && header[0] == 0x1B
        && header[1] == 'L' && header[2] == 'u' && header[3] == 'a'
        && header[4] == 0x51;
}

struct VmpGateFrame
{
    std::uintptr_t edi;
    std::uintptr_t esi;
    std::uintptr_t ebp;
    std::uintptr_t savedEsp;
    std::uintptr_t ebx;
    std::uintptr_t edx;
    std::uintptr_t ecx;
    std::uintptr_t eax;
    std::uint32_t flags;
    std::uintptr_t returnAddress;
    std::uintptr_t output;
    std::uintptr_t buffer;
    std::uint32_t size;
};

static_assert(sizeof(VmpGateFrame) == 0x34);

bool ScriptPath(std::uintptr_t address, std::string& text)
{
    return ReadText(address, text, 512) && ContainsI(text, ".lua")
        && (text.find('\\') != std::string::npos
            || text.find('/') != std::string::npos);
}

std::uintptr_t FindScriptPath(const VmpGateFrame& frame, std::string& text)
{
    const std::uintptr_t registers[]{frame.esi, frame.edi, frame.ecx,
        frame.eax, frame.edx, frame.ebx, frame.ebp};
    for(const std::uintptr_t candidate : registers)
    {
        if(ScriptPath(candidate, text))
            return candidate;
    }

    const auto stack = reinterpret_cast<std::uintptr_t>(&frame.returnAddress);
    for(std::size_t index = 1; index <= 48; ++index)
    {
        std::uintptr_t candidate{};
        if(ReadBytes(stack + index * sizeof(candidate), &candidate,
            sizeof(candidate)) && ScriptPath(candidate, text))
        {
            return candidate;
        }
    }
    return 0;
}

void __cdecl InspectVmpGate(const VmpGateFrame* frame)
{
    constexpr std::uintptr_t MaxLuaBuffer = 128u * 1024u * 1024u;
    if(!frame)
        return;
    std::uintptr_t buffer = frame->buffer;
    std::uint32_t size = frame->size;
    const std::uintptr_t alternateBuffer = frame->edi;
    const auto alternateSize = static_cast<std::uint32_t>(frame->ebx);
    if((size < 5 || size > MaxLuaBuffer || !LuaHeader(buffer))
        && alternateSize >= 5 && alternateSize <= MaxLuaBuffer
        && LuaHeader(alternateBuffer))
    {
        buffer = alternateBuffer;
        size = alternateSize;
    }
    const std::uint32_t hit = g_gateHits.fetch_add(1,
        std::memory_order_relaxed);
    if(hit < 4)
    {
        wchar_t message[384]{};
        swprintf_s(message,
            L"[lua-hook] VMP gate hit: stack_buffer=0x%08lX stack_size=%lu "
            L"ESI=0x%08lX EDI=0x%08lX EBX=%lu",
            static_cast<unsigned long>(frame->buffer), frame->size,
            static_cast<unsigned long>(frame->esi),
            static_cast<unsigned long>(frame->edi),
            static_cast<unsigned long>(frame->ebx));
        Log::Write(message);
    }
    if(size < 5 || size > MaxLuaBuffer || !LuaHeader(buffer))
        return;

    std::string pathText;
    const std::uintptr_t path = FindScriptPath(*frame, pathText);
    if(!path)
        return;
    if(ContainsI(pathText, "province_ac"))
        DumpProvinceAc(buffer, size, path);
}

__declspec(naked) void VmpGateRelay()
{
    __asm
    {
        pushfd
        pushad
        mov eax, esp
        sub esp, 220h
        and esp, 0FFFFFFF0h
        mov [esp + 200h], eax
        fxsave [esp]
        push eax
        call InspectVmpGate
        add esp, 4
        fxrstor [esp]
        mov esp, [esp + 200h]
        popad
        popfd
        jmp dword ptr [g_vmpTarget]
    }
}

bool PatchVmpGate(std::uintptr_t gate)
{
    std::uint8_t opcode{};
    std::int32_t originalDisplacement{};
    if(!ReadBytes(gate, &opcode, sizeof(opcode)) || opcode != 0xE8
        || !ReadBytes(gate + 1, &originalDisplacement,
            sizeof(originalDisplacement)))
    {
        return false;
    }

    g_vmpTarget = gate + 5 + originalDisplacement;
    const std::int64_t relayDisplacement = reinterpret_cast<std::uintptr_t>(
        &VmpGateRelay) - static_cast<std::int64_t>(gate + 5);
    if(relayDisplacement < std::numeric_limits<std::int32_t>::min()
        || relayDisplacement > std::numeric_limits<std::int32_t>::max())
    {
        g_vmpTarget = 0;
        return false;
    }

    std::uint8_t patch[5]{0xE8};
    const auto displacement = static_cast<std::int32_t>(relayDisplacement);
    std::memcpy(patch + 1, &displacement, sizeof(displacement));
    DWORD protection{};
    if(!VirtualProtect(reinterpret_cast<void*>(gate), sizeof(patch),
        PAGE_EXECUTE_READWRITE, &protection))
    {
        g_vmpTarget = 0;
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(gate), patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(gate),
        sizeof(patch));
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(gate), sizeof(patch), protection,
        &ignored);
    return true;
}

DWORD WINAPI ScanThread(void* parameter)
{
    const HMODULE client = static_cast<HMODULE>(parameter);
    const SignatureScanner scanner(client);
    std::uintptr_t gate{};
    while(!(gate = scanner.Find(VmpGatePattern)))
        Sleep(RetryDelayMs);
    Log::Scan(L"Lua VMP argument gate", L"found", gate);
    const bool installed = PatchVmpGate(gate);
    Log::Scan(L"Lua VMP argument relay",
        installed ? L"hook_installed" : L"hook_failed", gate);
    if(!InstallLuaBridge(client))
        Log::Write(L"[lua-hook] bridge unavailable; VMP relay remains active");
    FreeLibrary(client);
    return 0;
}
}

bool StartLuaArgsHook(HMODULE client, std::wstring_view outputDirectory)
{
    if(!client || g_started.exchange(true))
        return false;
    if(!outputDirectory.empty())
    {
        g_dumpDirectory = std::wstring(outputDirectory) + L"\\ProvinceAC";
        CleanDumpDirectory();
    }
    HMODULE held{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(client), &held))
    {
        Log::Write(L"[lua-hook] unable to retain client.dll");
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, &ScanThread, held, 0, nullptr);
    if(!thread)
    {
        FreeLibrary(held);
        Log::Write(L"[lua-hook] unable to start signature scan");
        return false;
    }
    CloseHandle(thread);
    Log::Write(L"[lua-hook] strict VMP gate scan started; retrying every 1 ms");
    return true;
}
