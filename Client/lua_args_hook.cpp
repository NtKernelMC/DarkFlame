#include "lua_args_hook.h"

#include "logger.h"
#include "signature_scanner.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace
{
constexpr std::string_view TargetPattern = "E8 C2 4D 0A 00 8B 54 25 00";
constexpr std::string_view ReplacedPath =
    "[province]\\province_barbecue\\barbecue-sh.lua";
constexpr char Replacement[] =
    "outputChatBox('Hello Colleman! Kak tam VM Protect?')";
constexpr DWORD RetryDelayMs = 1;

std::atomic_bool g_started{};
void* g_originalCallTarget{};

struct RegisterFrame
{
    std::uint32_t edi{};
    std::uint32_t esi{};
    std::uint32_t ebp{};
    std::uint32_t savedEsp{};
    std::uint32_t ebx{};
    std::uint32_t edx{};
    std::uint32_t ecx{};
    std::uint32_t eax{};
    std::uint32_t eflags{};
    std::uint32_t returnAddress{};
    std::uint32_t arg1{};
    std::uint32_t arg2{};
    std::uint32_t arg3{};
    std::uint32_t arg4{};
    std::uint32_t arg5{};
};

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

bool LuaHeader(std::uintptr_t address)
{
    unsigned char header[5]{};
    return ReadBytes(address, header, sizeof(header)) && header[0] == 0x1B
        && header[1] == 'L' && header[2] == 'u' && header[3] == 'a'
        && header[4] == 0x51;
}

bool EqualsText(std::uintptr_t address, std::string_view expected)
{
    if(!Readable(address))
        return false;
    __try
    {
        const auto* text = reinterpret_cast<const char*>(address);
        for(std::size_t index = 0; index < expected.size(); ++index)
        {
            if(text[index] != expected[index])
                return false;
        }
        return text[expected.size()] == '\0';
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool TextPreview(std::uintptr_t address, std::size_t available,
    wchar_t* output, std::size_t capacity)
{
    if(!Readable(address) || !output || capacity < 2 || !available)
        return false;
    const std::size_t limit = available < capacity - 1 ? available : capacity - 1;
    std::size_t length{};
    bool ended{};
    __try
    {
        const auto* text = reinterpret_cast<const unsigned char*>(address);
        for(; length < limit; ++length)
        {
            const unsigned char value = text[length];
            if(!value)
            {
                ended = true;
                break;
            }
            if(value == '\r' || value == '\n' || value == '\t')
                output[length] = L' ';
            else if(value >= 0x20 && value <= 0x7E)
                output[length] = static_cast<wchar_t>(value);
            else if(value >= 0x80)
                output[length] = L'.';
            else
                return false;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        output[length] = L'\0';
        return length >= 4;
    }
    output[length] = L'\0';
    return length >= 4 && (ended || length == limit);
}

void InspectValue(const wchar_t* stage, const wchar_t* name, std::uintptr_t value,
    std::size_t available, std::uintptr_t sizeHint = 0)
{
    if(!Readable(value))
        return;
    wchar_t message[360]{};
    if(LuaHeader(value))
    {
        swprintf_s(message,
            L"[lua-hook:%ls] %ls: LuaQ bytecode @ 0x%08lX, size_hint=%lu",
            stage, name, static_cast<unsigned long>(value),
            static_cast<unsigned long>(sizeHint));
        Log::Write(message);
        return;
    }
    wchar_t preview[193]{};
    if(!TextPreview(value, available, preview, sizeof(preview) / sizeof(preview[0])))
        return;
    swprintf_s(message, L"[lua-hook:%ls] %ls text @ 0x%08lX: \"%ls\"",
        stage, name, static_cast<unsigned long>(value), preview);
    Log::Write(message);
}

void LogFrame(const wchar_t* stage, RegisterFrame* frame)
{
    if(!frame)
        return;
    wchar_t message[320]{};
    swprintf_s(message,
        L"[lua-hook:%ls] ret=0x%08lX EAX=%08lX ECX=%08lX EDX=%08lX "
        L"EBX=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
        stage, frame->returnAddress, frame->eax, frame->ecx, frame->edx,
        frame->ebx, frame->ebp, frame->esi, frame->edi);
    Log::Write(message);
    swprintf_s(message, L"[lua-hook:%ls] luaVM: EAX=0x%08lX stack-arg1=0x%08lX",
        stage, frame->eax, frame->arg2);
    Log::Write(message);
    swprintf_s(message,
        L"[lua-hook:%ls] stack: outer_ret=0x%08lX arg1=0x%08lX "
        L"arg2=0x%08lX arg3=0x%08lX arg4=0x%08lX",
        stage, frame->arg1, frame->arg2, frame->arg3,
        frame->arg4, frame->arg5);
    Log::Write(message);

    constexpr std::size_t PreviewBytes = 192;
    constexpr std::uintptr_t MaxLuaBuffer = 128u * 1024u * 1024u;
    const std::uintptr_t buffer = LuaHeader(frame->edi) ? frame->edi : frame->arg3;
    const std::uintptr_t size = frame->ebx >= 5 && frame->ebx <= MaxLuaBuffer
        ? frame->ebx : frame->arg4;
    if(LuaHeader(buffer) && size >= 5 && size <= MaxLuaBuffer)
    {
        swprintf_s(message,
            L"[lua-hook:%ls] resolved LuaQ: buffer=0x%08lX size=%lu "
            L"(EDI/stack-arg2, EBX/stack-arg3)", stage,
            static_cast<unsigned long>(buffer), static_cast<unsigned long>(size));
        Log::Write(message);
    }
    InspectValue(stage, L"buffer", buffer, size, size);
    InspectValue(stage, L"path(ESI)", frame->esi, PreviewBytes);
    InspectValue(stage, L"name(stack-arg4)", frame->arg5, PreviewBytes);

    if(EqualsText(frame->esi, ReplacedPath))
    {
        const std::uintptr_t replacement = reinterpret_cast<std::uintptr_t>(Replacement);
        constexpr std::uint32_t replacementSize = sizeof(Replacement) - 1;
        frame->edi = replacement;
        frame->ebx = replacementSize;
        frame->arg3 = replacement;
        frame->arg4 = replacementSize;
        swprintf_s(message,
            L"[lua-hook:%ls] replaced sound-c.lua: buffer=0x%08lX size=%lu luaVM=0x%08lX",
            stage, static_cast<unsigned long>(replacement),
            static_cast<unsigned long>(replacementSize), frame->eax);
        Log::Write(message);
    }
}

void __cdecl LogEntryFrame(RegisterFrame* frame)
{
    LogFrame(L"entry", frame);
}

void __declspec(naked) HookCall()
{
    __asm
    {
        pushfd
        pushad
        push esp
        call LogEntryFrame
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_originalCallTarget]
    }
}

bool Install(std::uintptr_t address)
{
    std::uint8_t opcode{};
    std::int32_t originalDisplacement{};
    if(!ReadBytes(address, &opcode, sizeof(opcode)) || opcode != 0xE8
        || !ReadBytes(address + 1, &originalDisplacement, sizeof(originalDisplacement)))
        return false;
    g_originalCallTarget = reinterpret_cast<void*>(address + 5 + originalDisplacement);
    const std::int32_t hookDisplacement = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(&HookCall) - (address + 5));
    DWORD protection{};
    if(!VirtualProtect(reinterpret_cast<void*>(address), 5,
        PAGE_EXECUTE_READWRITE, &protection))
        return false;
    __try
    {
        *reinterpret_cast<volatile std::int32_t*>(address + 1) = hookDisplacement;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        DWORD ignored{};
        VirtualProtect(reinterpret_cast<void*>(address), 5, protection, &ignored);
        g_originalCallTarget = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 5);
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(address), 5, protection, &ignored);
    Log::Scan(L"Lua VM call", L"naked_call_hook_installed", address);
    return true;
}

DWORD WINAPI ScanThread(void* parameter)
{
    const HMODULE client = static_cast<HMODULE>(parameter);
    const SignatureScanner scanner(client);
    std::uintptr_t address{};
    while(!(address = scanner.Find(TargetPattern)))
        Sleep(RetryDelayMs);
    Log::Scan(L"Lua four-argument target", L"found", address);
    Install(address);
    FreeLibrary(client);
    return 0;
}
}

bool StartLuaArgsHook(HMODULE client)
{
    if(!client || g_started.exchange(true))
        return false;
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
    Log::Write(L"[lua-hook] target scan started; retrying every 1 ms");
    return true;
}
