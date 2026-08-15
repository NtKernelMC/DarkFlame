#include "netc_hooks.h"

#include "hook_utils.h"
#include "gui.h"
#include "logger.h"
#include "netc_signatures.h"
#include "signature_scanner.h"
#include "netc_bitstream.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using SendPacket = bool(__thiscall*)(void*, unsigned char, void*, int, int, int);
    using GetElement = void*(__cdecl*)(unsigned int);

    SRWLOCK g_installLock = SRWLOCK_INIT;
    SRWLOCK g_clientApiLock = SRWLOCK_INIT;
    SRWLOCK g_contextLock = SRWLOCK_INIT;
    SendPacket g_sendPacket{};
    GetElement g_getElement{};
    bool g_installed{};
    std::atomic_uint g_decodeFailures{};

    struct LuaContext
    {
        DWORD thread{};
        ULONGLONG tick{};
        char resource[128]{};
    };

    LuaContext g_luaContexts[8]{};

    constexpr unsigned char LuaEventPacket = 81;
    constexpr std::string_view GetElementPattern =
        "55 8B EC 8B 45 ? 3D ? ? ? ? 73 ? 8B 04 85";
    constexpr unsigned int MaxArguments = 512;
    constexpr unsigned int MaxStringBytes = 4 * 1024 * 1024;
    constexpr unsigned int MaxDepth = 10;

    using BitStreamReader = Netc::BitStream;

    std::string Escape(std::string_view value);

    bool Readable(const void* pointer, std::size_t size)
    {
        if(!pointer || !size)
            return pointer && !size;
        std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(pointer);
        if(cursor > std::numeric_limits<std::uintptr_t>::max() - size)
            return false;
        const std::uintptr_t finish = cursor + size;
        while(cursor < finish)
        {
            MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                sizeof(info)) || info.State != MEM_COMMIT
                || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            {
                return false;
            }
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(
                info.BaseAddress);
            if(base > std::numeric_limits<std::uintptr_t>::max() - info.RegionSize)
                return false;
            const std::uintptr_t end = base + info.RegionSize;
            if(end <= cursor)
                return false;
            cursor = (std::min)(end, finish);
        }
        return true;
    }

    template<class T>
    bool ReadValue(const void* base, std::size_t offset, T& value)
    {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(base);
        if(address > std::numeric_limits<std::uintptr_t>::max() - offset)
            return false;
        const void* source = reinterpret_cast<const void*>(address + offset);
        if(!Readable(source, sizeof(T)))
            return false;
        std::memcpy(&value, source, sizeof(T));
        return true;
    }

    bool ReadSString(const void* object, std::string& output,
        std::size_t limit = 256)
    {
        std::uint32_t length{};
        std::uint32_t capacity{};
        if(!ReadValue(object, 16, length) || !ReadValue(object, 20, capacity)
            || length > capacity || length > 1024 * 1024)
        {
            return false;
        }
        const char* text{};
        if(capacity <= 15)
            text = static_cast<const char*>(object);
        else if(!ReadValue(object, 0, text))
            return false;
        const std::size_t copied = (std::min)(limit,
            static_cast<std::size_t>(length));
        if(copied && !Readable(text, copied))
            return false;
        output.assign(text ? text : "", copied);
        return true;
    }

    std::string CurrentResource()
    {
        const DWORD thread = GetCurrentThreadId();
        const ULONGLONG now = GetTickCount64();
        std::string resource = "unknown";
        AcquireSRWLockShared(&g_contextLock);
        for(const auto& context : g_luaContexts)
        {
            if(context.thread == thread && context.resource[0]
                && now - context.tick <= 1000)
            {
                resource = context.resource;
                break;
            }
        }
        ReleaseSRWLockShared(&g_contextLock);
        return resource;
    }

    void* ResolveElement(unsigned int id)
    {
        return g_getElement && id < 0x20000 ? g_getElement(id) : nullptr;
    }

    std::string ElementExpression(unsigned int id, std::string_view resource)
    {
        void* entity = ResolveElement(id);
        if(!entity)
        {
            char fallback[48]{};
            std::snprintf(fallback, sizeof(fallback),
                "nil --[[unresolved netID=0x%05X]]", id);
            return fallback;
        }

        std::string type;
        ReadSString(static_cast<const char*>(entity) + 108, type, 64);
        if(type == "player")
        {
            unsigned char local{};
            if(ReadValue(entity, 1565, local) && local)
                return "localPlayer";
            char suffix[32]{};
            std::snprintf(suffix, sizeof(suffix),
                " --[[netID=0x%05X]]", id);
            return std::string("player") + suffix;
        }
        if(type == "resource")
        {
            if(resource != "unknown")
                return "getResourceRootElement(getResourceFromName("
                    + Escape(resource) + "))";
            return "resourceRoot";
        }

        std::string label = type.empty() || type == "unknown" ? "element" : type;
        char suffix[32]{};
        std::snprintf(suffix, sizeof(suffix), " --[[netID=0x%05X]]", id);
        return label + suffix;
    }

    std::string Escape(std::string_view value)
    {
        std::string result;
        result.reserve(value.size() + 2);
        result.push_back('"');
        for(unsigned char ch : value)
        {
            switch(ch)
            {
                case '\\': result += "\\\\"; break;
                case '"': result += "\\\""; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if(ch >= 32)
                        result.push_back(static_cast<char>(ch));
                    else
                    {
                        char escaped[5]{};
                        std::snprintf(escaped, sizeof(escaped), "\\x%02X", ch);
                        result += escaped;
                    }
                    break;
            }
        }
        result.push_back('"');
        return result;
    }

    std::string Number(double value)
    {
        char text[64]{};
        if(std::isfinite(value))
            std::snprintf(text, sizeof(text), "%.14g", value);
        else if(std::isnan(value))
            std::snprintf(text, sizeof(text), "(0/0)");
        else
            std::snprintf(text, sizeof(text),
                value < 0 ? "-math.huge" : "math.huge");
        return text;
    }

    bool ReadString(BitStreamReader& reader, unsigned int length,
        bool aligned, std::string& output)
    {
        if(length > MaxStringBytes)
            return false;
        if(aligned)
            reader.Align();
        std::string value(length, '\0');
        if(length && !reader.Bytes(value.data(), static_cast<int>(length)))
            return false;
        constexpr std::size_t DisplayLimit = 2048;
        output = Escape(value.substr(0, DisplayLimit));
        if(value.size() > DisplayLimit)
            output += "...<" + std::to_string(value.size()) + " bytes>";
        return true;
    }

    bool ReadArgument(BitStreamReader& reader, unsigned int depth,
        std::vector<unsigned int>& tables, std::string& output);

    bool ReadArguments(BitStreamReader& reader, unsigned int depth,
        std::vector<unsigned int>& tables, std::string& output, bool table)
    {
        if(depth > MaxDepth)
            return false;
        unsigned int count{};
        if(!reader.Compressed(count) || count > MaxArguments)
            return false;
        const unsigned int tableId = static_cast<unsigned int>(tables.size());
        tables.push_back(tableId);
        output += table ? "{" : "(";
        if(table)
        {
            for(unsigned int i{}; i < count; i += 2)
            {
                std::string key;
                std::string value;
                if(!ReadArgument(reader, depth + 1, tables, key))
                    return false;
                if(i + 1 < count)
                {
                    if(!ReadArgument(reader, depth + 1, tables, value))
                        return false;
                }
                else
                {
                    value = "nil";
                }
                if(i)
                    output += ", ";
                output += "[" + key + "] = " + value;
            }
        }
        else
        {
            for(unsigned int i{}; i < count; ++i)
            {
                if(i)
                    output += ", ";
                if(!ReadArgument(reader, depth + 1, tables, output))
                    return false;
            }
        }
        output += table ? "}" : ")";
        return true;
    }

    bool ReadArgument(BitStreamReader& reader, unsigned int depth,
        std::vector<unsigned int>& tables, std::string& output)
    {
        unsigned char type{};
        if(depth > MaxDepth || !reader.Bits(&type, 4))
            return false;
        switch(type & 0x0F)
        {
            case 0:
                output += "nil";
                return true;
            case 1:
            {
                bool value{};
                if(!reader.Bit(value))
                    return false;
                output += value ? "true" : "false";
                return true;
            }
            case 2:
            case 7:
            {
                unsigned int element{};
                if(!reader.Bits(&element, 17))
                    return false;
                output += ElementExpression(element, CurrentResource());
                return true;
            }
            case 3:
            {
                bool precise{};
                if(!reader.Bit(precise))
                    return false;
                if(!precise)
                {
                    int value{};
                    if(!reader.Compressed(value))
                        return false;
                    output += std::to_string(value);
                    return true;
                }
                bool wide{};
                if(!reader.Bit(wide))
                    return false;
                if(wide)
                {
                    double value{};
                    if(!reader.Double(value))
                        return false;
                    output += Number(value);
                }
                else
                {
                    float value{};
                    if(!reader.Float(value))
                        return false;
                    output += Number(value);
                }
                return true;
            }
            case 4:
            {
                unsigned short length{};
                return reader.Compressed(length)
                    && ReadString(reader, length, false, output);
            }
            case 5:
                return ReadArguments(reader, depth, tables, output, true);
            case 9:
            {
                unsigned int tableId{};
                if(!reader.Compressed(tableId) || tableId >= tables.size())
                    return false;
                output += "nil --[[table-ref#" + std::to_string(tableId) + "]]";
                return true;
            }
            case 10:
            {
                unsigned int length{};
                return reader.Compressed(length)
                    && ReadString(reader, length, true, output);
            }
            default:
                return false;
        }
    }

    bool DecodeLuaEvent(BitStreamReader& reader, std::string& row)
    {
        unsigned short nameLength{};
        if(!reader.Compressed(nameLength) || !nameLength || nameLength > 512)
            return false;
        std::string name(nameLength, '\0');
        if(!reader.Bytes(name.data(), nameLength))
            return false;
        unsigned int element{};
        if(!reader.Bits(&element, 17))
            return false;
        std::vector<unsigned int> tables;
        std::string arguments;
        if(!ReadArguments(reader, 0, tables, arguments, false))
            return false;
        const std::string resource = CurrentResource();
        const std::string sourceElement = ElementExpression(element, resource);
        row = "[" + resource + "] triggerServerEvent(" + Escape(name)
            + ", " + sourceElement;
        if(arguments.size() > 2)
            row += ", " + arguments.substr(1, arguments.size() - 2);
        row += ")";
        return true;
    }

    bool TryDecodeLuaEvent(void* stream, std::string& row)
    {
        int originalOffset{};
        bool offsetKnown{};
        bool decoded{};
        __try
        {
            auto& reader = *static_cast<BitStreamReader*>(stream);
            originalOffset = reader.Offset();
            offsetKnown = true;
            reader.Offset(0);
            decoded = DecodeLuaEvent(reader, row);
            reader.Offset(originalOffset);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            if(offsetKnown)
            {
                __try
                {
                    static_cast<BitStreamReader*>(stream)->Offset(originalOffset);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            decoded = false;
        }
        return decoded;
    }

    void ObservePacket(unsigned char packetId, void* bitStream)
    {
        if(packetId != LuaEventPacket || !bitStream)
            return;
        try
        {
            std::string row;
            if(TryDecodeLuaEvent(bitStream, row))
            {
                GuiAppendEvent(row);
                ClearNetcLuaCallContext();
                return;
            }
        }
        catch(...)
        {
        }
        const unsigned int failure = g_decodeFailures.fetch_add(1);
        if(failure < 3)
            Log::Write(L"[netc] outgoing Lua event decode failed");
        ClearNetcLuaCallContext();
    }

    void __fastcall HookVpnBypass(DWORD*, void*, char) {}
    bool __cdecl HookIsNetworkConnected(DWORD*, DWORD*) { return false; }
    bool __fastcall HookIsNotViolationCode(void*, void*, unsigned int) { return true; }
    int __fastcall HookExecuteSecurityViolationKick(void*, void*) { return 0; }
    int __fastcall HookSendClientKick(void*, void*, char) { return 1; }
    int __cdecl HookSendLogger(int, void*, int, int, void*) { return 0; }
    int __fastcall HookSetClientKick(void*, void*, void*, void*, char, int) { return 0; }
    void __fastcall HookVfB00Z00Scanner(void*, void*, int) {}
    int __fastcall HookSetClientKickNew(void*, void*, char) { return 0; }
    int __fastcall HookScanModuleIntegrity(void*, void*, DWORD*) { return 0; }

    bool __fastcall HookSendPacket(void* self, void*, unsigned char packetId,
        void* bitStream, int priority, int reliability, int ordering)
    {
        ObservePacket(packetId, bitStream);
        const bool blocked = packetId == 34 || packetId == 91
            || packetId == 92 || packetId == 94;
        return blocked || g_sendPacket(self, packetId, bitStream,
            priority, reliability, ordering);
    }
}

bool InitializeNetcClientApi(HMODULE client)
{
    if(g_getElement)
        return true;
    AcquireSRWLockExclusive(&g_clientApiLock);
    if(g_getElement)
    {
        ReleaseSRWLockExclusive(&g_clientApiLock);
        return true;
    }
    const std::uintptr_t address = SignatureScanner(client).Find(GetElementPattern);
    g_getElement = reinterpret_cast<GetElement>(address);
    Log::Scan(L"CElementIDs::GetElement", address ? L"found" : L"not_found",
        address);
    ReleaseSRWLockExclusive(&g_clientApiLock);
    return g_getElement != nullptr;
}

void SetNetcLuaCallContext(std::string_view resource)
{
    if(resource.empty())
        resource = "unknown";
    const DWORD thread = GetCurrentThreadId();
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_contextLock);
    LuaContext* selected = &g_luaContexts[0];
    for(auto& context : g_luaContexts)
    {
        if(context.thread == thread || !context.thread || now - context.tick > 1000)
        {
            selected = &context;
            break;
        }
        if(context.tick < selected->tick)
            selected = &context;
    }
    const std::size_t length = (std::min)(resource.size(), sizeof(selected->resource) - 1);
    std::memcpy(selected->resource, resource.data(), length);
    selected->resource[length] = '\0';
    selected->thread = thread;
    selected->tick = now;
    ReleaseSRWLockExclusive(&g_contextLock);
}

void ClearNetcLuaCallContext()
{
    const DWORD thread = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_contextLock);
    for(auto& context : g_luaContexts)
    {
        if(context.thread != thread)
            continue;
        context = {};
        break;
    }
    ReleaseSRWLockExclusive(&g_contextLock);
}

bool InstallNetcHooks(HMODULE netc)
{
    AcquireSRWLockExclusive(&g_installLock);
    if (g_installed)
    {
        ReleaseSRWLockExclusive(&g_installLock);
        return true;
    }
    if (!netc)
    {
        ReleaseSRWLockExclusive(&g_installLock);
        return false;
    }

    const SignatureScanner scanner(netc);
    std::array hooks{
        HookUtils::Hook{L"AC__IsVpnEnabled", NetcSignatures::VpnBypass,
            reinterpret_cast<void*>(&HookVpnBypass)},
        HookUtils::Hook{L"AC__IsNetworkConnected", NetcSignatures::IsNetworkConnected,
            reinterpret_cast<void*>(&HookIsNetworkConnected)},
        HookUtils::Hook{L"AC__IsNotViolationCode", NetcSignatures::IsNotViolationCode,
            reinterpret_cast<void*>(&HookIsNotViolationCode)},
        HookUtils::Hook{L"AC_ExecuteSecurityViolationKick", NetcSignatures::ExecuteSecurityViolationKick,
            reinterpret_cast<void*>(&HookExecuteSecurityViolationKick)},
        HookUtils::Hook{L"AC__SendClientKick", NetcSignatures::SendClientKick,
            reinterpret_cast<void*>(&HookSendClientKick)},
        HookUtils::Hook{L"AC__SendLoggerToServerMtaDev", NetcSignatures::SendLogger,
            reinterpret_cast<void*>(&HookSendLogger)},
        HookUtils::Hook{L"AC_SetClientKick", NetcSignatures::SetClientKick,
            reinterpret_cast<void*>(&HookSetClientKick)},
        HookUtils::Hook{L"AC__VfB00_Z00Scanner", NetcSignatures::VfB00Z00Scanner,
            reinterpret_cast<void*>(&HookVfB00Z00Scanner)},
        HookUtils::Hook{L"AC__SetClientKickNew", NetcSignatures::SetClientKickNew,
            reinterpret_cast<void*>(&HookSetClientKickNew)},
        HookUtils::Hook{L"AC_HandleSelfFileIntegrityResultPacket", NetcSignatures::ScanModuleIntegrity,
            reinterpret_cast<void*>(&HookScanModuleIntegrity)},
        HookUtils::Hook{L"CNet__SendPacket", NetcSignatures::SendPacket,
            reinterpret_cast<void*>(&HookSendPacket), reinterpret_cast<void**>(&g_sendPacket)}
    };
    g_installed = HookUtils::Install(scanner, hooks);
    if (!g_installed)
        g_sendPacket = nullptr;
    ReleaseSRWLockExclusive(&g_installLock);
    return g_installed;
}
