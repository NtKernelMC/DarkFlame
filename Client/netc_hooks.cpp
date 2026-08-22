#include "netc_hooks.h"

#include "hook_utils.h"
#include "gui.h"
#include "logger.h"
#include "memory_utils.h"
#include "netc_signatures.h"
#include "signature_scanner.h"
#include "netc_bitstream.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace MemoryUtil;

    using SendPacket = bool(__thiscall*)(void*, unsigned char, void*, int, int, int);
    using RakPeerSend = char(__thiscall*)(void*, void*, int, int, char, int,
        short, char);
    using GetElement = void*(__cdecl*)(unsigned int);
    using DiskDriveSerial = bool(__cdecl*)(const char*, const char*, int);

    SRWLOCK g_installLock = SRWLOCK_INIT;
    SRWLOCK g_clientApiLock = SRWLOCK_INIT;
    SRWLOCK g_contextLock = SRWLOCK_INIT;
    SendPacket g_sendPacket{};
    RakPeerSend g_rakPeerSend{};
    GetElement g_getElement{};
    DiskDriveSerial g_diskDriveSerial{};
    HMODULE g_netcModule{};
    HMODULE g_clientApiModule{};
    bool g_installed{};
    bool g_serialInstalled{};
    bool g_randomSerialInstalled{};
    bool g_randomSerialUnavailable{};
    std::atomic_bool g_setSerial{};
    std::atomic_bool g_randomSerial{};
    std::atomic_bool g_randomSerialSpent{};
    std::atomic_bool g_serialLogged{};
    std::array<unsigned char, 32> g_publicSerialEncoded{};
    std::array<char, 33> g_publicSerial{};
    std::array<char, 64> g_randomDriveSerial{};
    std::array<char, 64> g_randomDriveModel{};
    std::atomic_uint g_decodeFailures{};

    std::mt19937 RandomEngine()
    {
        std::random_device device;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        std::seed_seq seed{static_cast<unsigned int>(device()),
            static_cast<unsigned int>(device()),
            static_cast<unsigned int>(GetCurrentProcessId()),
            static_cast<unsigned int>(GetCurrentThreadId()),
            static_cast<unsigned int>(GetTickCount()),
            static_cast<unsigned int>(counter.LowPart),
            static_cast<unsigned int>(counter.HighPart)};
        return std::mt19937(seed);
    }

    void GenerateRandomDrive()
    {
        auto engine = RandomEngine();
        std::uniform_int_distribution<int> letter(0, 25);
        std::uniform_int_distribution<int> digit(0, 9);
        std::uniform_int_distribution<int> maker(0, 4);
        constexpr std::string_view manufacturers[] = {
            "KINGSTON", "SAMSUNG", "WD", "SEAGATE", "TOSHIBA"};
        char* serial = g_randomDriveSerial.data();
        *serial++ = static_cast<char>('A' + letter(engine));
        *serial++ = static_cast<char>('A' + letter(engine));
        for(int index{}; index < 12; ++index)
            *serial++ = static_cast<char>('0' + digit(engine));
        *serial++ = static_cast<char>('A' + letter(engine));
        *serial++ = static_cast<char>('A' + letter(engine));
        *serial = '\0';

        char code[14]{};
        int position{};
        code[position++] = static_cast<char>('A' + letter(engine));
        code[position++] = static_cast<char>('A' + letter(engine));
        for(int index{}; index < 3; ++index)
            code[position++] = static_cast<char>('0' + digit(engine));
        code[position++] = static_cast<char>('A' + letter(engine));
        for(int index{}; index < 5; ++index)
            code[position++] = static_cast<char>('0' + digit(engine));
        code[position++] = 'G';
        code[position++] = 'B';
        std::snprintf(g_randomDriveModel.data(), g_randomDriveModel.size(),
            "%s %s", manufacturers[maker(engine)].data(), code);
    }

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

    struct RakBitStreamView
    {
        std::uint32_t bitsUsed;
        std::uint32_t bitsAllocated;
        std::uint32_t readOffset;
        unsigned char* data;
    };

    bool ApplyPublicSerial(void* bitStream)
    {
        if(!g_setSerial.load(std::memory_order_acquire)
            || !Readable(bitStream, sizeof(RakBitStreamView)))
            return false;
        RakBitStreamView stream{};
        std::memcpy(&stream, bitStream, sizeof(stream));
        if(!stream.data || stream.bitsUsed > stream.bitsAllocated)
            return false;
        const std::size_t bytes = (static_cast<std::size_t>(stream.bitsUsed) + 7) >> 3;
        if(bytes < 35 || bytes > 4 * 1024 * 1024 || !Readable(stream.data, bytes))
            return false;
        constexpr unsigned int RakPacketBase = 99;
        constexpr unsigned int PlayerJoinData = 4;
        constexpr unsigned int Reserved13JoinData = 16;
        const unsigned int raw = stream.data[0];
        if(raw != RakPacketBase + PlayerJoinData
            && raw != RakPacketBase + Reserved13JoinData)
            return false;
        if(!Writable(stream.data + 3, g_publicSerialEncoded.size()))
            return false;
        std::memcpy(stream.data + 3, g_publicSerialEncoded.data(),
            g_publicSerialEncoded.size());
        if(!g_serialLogged.exchange(true, std::memory_order_acq_rel))
        {
            const std::string value(g_publicSerial.data());
            Log::Write(L"[serial] public serial substituted => "
                + std::wstring(value.begin(), value.end()));
        }
        return true;
    }

    char __fastcall HookRakPeerSend(void* self, void*, void* bitStream,
        int priority, int reliability, char orderingChannel, int targetIp,
        short targetPort, char broadcast)
    {
        ApplyPublicSerial(bitStream);
        return g_rakPeerSend(self, bitStream, priority, reliability,
            orderingChannel, targetIp, targetPort, broadcast);
    }

    bool __cdecl HookDiskDriveSerial(const char* driveSerial,
        const char* driveModel, int busType)
    {
        if(busType != 12)
            return g_diskDriveSerial(driveSerial, driveModel, busType);
        const bool result = g_diskDriveSerial(g_randomDriveSerial.data(),
            g_randomDriveModel.data(), busType);
        if(result && !g_randomSerialSpent.exchange(true,
            std::memory_order_acq_rel))
        {
            const std::string text = "[serial] random disk identity used once: "
                + std::string(g_randomDriveSerial.data()) + " / "
                + std::string(g_randomDriveModel.data());
            Log::Write(std::wstring(text.begin(), text.end()));
        }
        return result;
    }

    std::array<HookUtils::Hook, 11>& Hooks()
    {
        static std::array hooks{
            HookUtils::Hook{L"AC__IsVpnEnabled", NetcSignatures::VpnBypass,
                reinterpret_cast<void*>(&HookVpnBypass)},
            HookUtils::Hook{L"AC__IsNetworkConnected", NetcSignatures::IsNetworkConnected,
                reinterpret_cast<void*>(&HookIsNetworkConnected)},
            HookUtils::Hook{L"AC__IsNotViolationCode", NetcSignatures::IsNotViolationCode,
                reinterpret_cast<void*>(&HookIsNotViolationCode)},
            HookUtils::Hook{L"AC_ExecuteSecurityViolationKick",
                NetcSignatures::ExecuteSecurityViolationKick,
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
            HookUtils::Hook{L"AC_HandleSelfFileIntegrityResultPacket",
                NetcSignatures::ScanModuleIntegrity,
                reinterpret_cast<void*>(&HookScanModuleIntegrity)},
            HookUtils::Hook{L"CNet__SendPacket", NetcSignatures::SendPacket,
                reinterpret_cast<void*>(&HookSendPacket),
                reinterpret_cast<void**>(&g_sendPacket)}
        };
        return hooks;
    }

    std::array<HookUtils::Hook, 1>& SerialHooks()
    {
        static std::array hooks{
            HookUtils::Hook{L"RakPeer_SendBitStreamOrBuffer",
                NetcSignatures::RakPeerSendBitStreamOrBuffer,
                reinterpret_cast<void*>(&HookRakPeerSend),
                reinterpret_cast<void**>(&g_rakPeerSend)}
        };
        return hooks;
    }

    std::array<HookUtils::Hook, 1>& RandomSerialHooks()
    {
        static std::array hooks{
            HookUtils::Hook{L"DiskDriveSerial", NetcSignatures::DiskDriveSerial,
                reinterpret_cast<void*>(&HookDiskDriveSerial),
                reinterpret_cast<void**>(&g_diskDriveSerial)}
        };
        return hooks;
    }
}

bool ConfigureNetcHooks(bool setSerial, bool randomSerial,
    std::string_view publicSerial)
{
    if(setSerial && randomSerial)
        return false;
    g_randomSerial.store(randomSerial, std::memory_order_release);
    g_randomSerialSpent.store(false, std::memory_order_release);
    if(randomSerial)
        GenerateRandomDrive();
    if(!setSerial)
    {
        g_setSerial.store(false, std::memory_order_release);
        g_serialLogged.store(false, std::memory_order_release);
        return true;
    }
    if(publicSerial.size() != g_publicSerialEncoded.size())
        return false;
    for(std::size_t index{}; index < publicSerial.size(); ++index)
    {
        const unsigned char ch = static_cast<unsigned char>(publicSerial[index]);
        if(!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')))
            return false;
        g_publicSerial[index] = static_cast<char>(ch);
        g_publicSerialEncoded[index] = static_cast<unsigned char>(ch ^ index
            ^ 0xD1 ^ (1u << (index & 7)));
    }
    g_publicSerial.back() = '\0';
    g_serialLogged.store(false, std::memory_order_release);
    g_setSerial.store(true, std::memory_order_release);
    return true;
}

bool InitializeNetcClientApi(HMODULE client)
{
    if(g_getElement && g_clientApiModule == client)
        return true;
    AcquireSRWLockExclusive(&g_clientApiLock);
    if(g_getElement && g_clientApiModule == client)
    {
        ReleaseSRWLockExclusive(&g_clientApiLock);
        return true;
    }
    const std::uintptr_t address = SignatureScanner(client).Find(GetElementPattern);
    g_getElement = reinterpret_cast<GetElement>(address);
    g_clientApiModule = address ? client : nullptr;
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
        if(g_netcModule == netc)
        {
            if(g_randomSerialSpent.load(std::memory_order_acquire)
                && g_randomSerialInstalled)
            {
                HookUtils::Remove(RandomSerialHooks());
                g_randomSerialInstalled = false;
                Log::Write(L"[serial] one-shot disk identity hook removed");
            }
            const bool coreReady = HookUtils::Repair(Hooks());
            const bool serialReady = !g_setSerial.load(std::memory_order_acquire)
                || (g_serialInstalled && HookUtils::Repair(SerialHooks()));
            const bool randomReady = !g_randomSerial.load(std::memory_order_acquire)
                || g_randomSerialSpent.load(std::memory_order_acquire)
                || g_randomSerialUnavailable
                || (g_randomSerialInstalled && HookUtils::Repair(RandomSerialHooks()));
            if(coreReady && serialReady && randomReady)
            {
                ReleaseSRWLockExclusive(&g_installLock);
                return true;
            }
        }
        HookUtils::Remove(Hooks());
        HookUtils::Remove(SerialHooks());
        HookUtils::Remove(RandomSerialHooks());
        g_installed = false;
        g_serialInstalled = false;
        g_randomSerialInstalled = false;
        g_randomSerialUnavailable = false;
        g_netcModule = nullptr;
        g_sendPacket = nullptr;
        g_rakPeerSend = nullptr;
        g_diskDriveSerial = nullptr;
    }
    if (!netc)
    {
        ReleaseSRWLockExclusive(&g_installLock);
        return false;
    }

    const SignatureScanner scanner(netc);
    g_serialInstalled = false;
    g_randomSerialUnavailable = false;
    g_installed = HookUtils::Install(scanner, Hooks());
    if(g_installed && g_setSerial.load(std::memory_order_acquire))
    {
        g_serialInstalled = HookUtils::Install(scanner, SerialHooks());
        if(!g_serialInstalled)
        {
            HookUtils::Remove(SerialHooks());
            HookUtils::Remove(Hooks());
            g_installed = false;
        }
    }
    if(g_installed && g_randomSerial.load(std::memory_order_acquire)
        && !g_randomSerialSpent.load(std::memory_order_acquire))
    {
        g_randomSerialInstalled = HookUtils::Install(scanner, RandomSerialHooks());
        if(!g_randomSerialInstalled)
        {
            g_randomSerialUnavailable = true;
            Log::Write(L"[serial] random disk identity hook unavailable");
        }
    }
    if (!g_installed)
    {
        g_sendPacket = nullptr;
        g_rakPeerSend = nullptr;
        g_diskDriveSerial = nullptr;
    }
    else
        g_netcModule = netc;
    ReleaseSRWLockExclusive(&g_installLock);
    return g_installed;
}

void ResetNetcHooks(HMODULE netc)
{
    AcquireSRWLockExclusive(&g_installLock);
    if(g_installed && (!netc || netc == g_netcModule))
    {
        HookUtils::Remove(Hooks());
        HookUtils::Remove(SerialHooks());
        HookUtils::Remove(RandomSerialHooks());
        g_installed = false;
        g_serialInstalled = false;
        g_randomSerialInstalled = false;
        g_randomSerialUnavailable = false;
        g_netcModule = nullptr;
        g_sendPacket = nullptr;
        g_rakPeerSend = nullptr;
        g_diskDriveSerial = nullptr;
    }
    ReleaseSRWLockExclusive(&g_installLock);
}

void ResetNetcClientApi(HMODULE client)
{
    AcquireSRWLockExclusive(&g_clientApiLock);
    if(!client || client == g_clientApiModule)
    {
        g_getElement = nullptr;
        g_clientApiModule = nullptr;
    }
    ReleaseSRWLockExclusive(&g_clientApiLock);
}
