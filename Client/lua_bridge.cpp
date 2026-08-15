#include "lua_bridge.h"

#include "gui.h"
#include "logger.h"
#include "netc_hooks.h"
#include "signature_scanner.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr int LuaGlobalsIndex = -10002;
constexpr int LuaRegistryIndex = -10000;
constexpr std::string_view IsNameAllowedPattern =
    "55 8B EC 8B 4D ? B8 ? ? ? ? 53 56 33 DB";
constexpr std::string_view CallHookPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 81 EC ? ? ? ? A1 ? ? ? ? "
    "33 C5 89 45 ? 53 56 57 50 8D 45 ? 64 A3 ? ? ? ? 80 3D";
constexpr std::string_view LuaPCallPattern =
    "55 8B EC 83 EC ? 53 8B 5D ? 56 8B 75 ? 57 8D 43";
constexpr std::string_view LuaPushCClosurePattern =
    "55 8B EC 56 8B 75 ? 8B 4E ? 8B 41 ? 3B 41 ? 72 ? 56 E8 ? ? ? ? "
    "83 C4 ? 8B 46 ? 2B 46";
constexpr std::string_view LuaSetFieldPattern =
    "55 8B EC 83 EC ? 53 56 8B 75 ? 57 8B 46 ? 2B 46";
constexpr std::string_view LuaPushBooleanPattern =
    "55 8B EC 8B 55 ? 33 C0 39 45";
constexpr std::string_view LuaToBooleanPattern =
    "55 8B EC FF 75 ? FF 75 ? E8 ? ? ? ? 83 C4 ? 8B 48 ? 85 C9";
constexpr std::string_view LuaToLStringPattern =
    "55 8B EC 56 FF 75 ? 8B 75 ? 56 E8 ? ? ? ? 8B C8";
constexpr std::string_view LuaPushStringPattern =
    "55 8B EC 8B 45 ? 85 C0 75 ? 8B 55";
constexpr std::string_view LuaGetTopPattern =
    "55 8B EC 8B 4D ? 8B 41 ? 2B 41 ? C1 F8";
constexpr std::string_view LuaGetFieldPattern =
    "55 8B EC 83 EC ? 56 8B 75 ? 57 FF 75";
constexpr std::string_view LuaSetTopPattern =
    "55 8B EC 8B 55 ? 56 8B 75 ? 57 8B 7A";
constexpr std::string_view LuaNewThreadPattern =
    "55 8B EC 56 8B 75 ? 8B 4E ? 8B 41 ? 3B 41 ? 72 ? 56 E8 ? ? ? ? "
    "83 C4 ? 56";
constexpr std::string_view LuaLRefPattern =
    "55 8B EC 56 8B 75 ? 57 8B 7D ? 8D 87 ? ? ? ? 3D ? ? ? ? 77 ? "
    "56 E8 ? ? ? ? 47 83 C4 ? 03 F8 6A";
constexpr std::string_view LuaLUnrefPattern =
    "55 8B EC 53 8B 5D ? 85 DB 78";
constexpr std::string_view LuaFunctionRegistryPattern =
    "55 8B EC 83 EC ? 56 8B 75 ? 3B 35";
constexpr std::string_view GetVirtualMachinePattern =
    "55 8B EC 83 EC ? 53 57 8B 7D ? 8B D9 85 FF";
constexpr std::string_view LuaManagerLoadPattern =
    "8B 0D ? ? ? ? 57 C7 45 ? ? ? ? ? E8 ? ? ? ? 85 C0 0F 84 ? ? ? ? "
    "83 78 ? ?";

constexpr char LuaBootstrap[] = R"DFLUA(
local O,G=false,0
local TS,TL=triggerServerEvent,triggerLatentServerEvent
local R=getThisResource()
local Global=_G
local PCall,Raise,Kind=pcall,error,type
local RawSet,SetMeta=rawset,setmetatable
local Scope=dfTrapScope

local function restore(g)
    if g~=G then return end
    hideFunctionCall(false)
end

local function injectError(kind,value)
    dfEmit('inject',value,0)
    outputChatBox('[DarkFlame] '..kind..': '..tostring(value),255,64,64)
end

local function emitEvent(sourceResource,functionName,allowed,file,line,...)
    local args={...}
    local resourceName=sourceResource and getResourceName(sourceResource)
    if resourceName=='province_afktimer'
        or tostring(args[1])=='inventory:getPlayersObjectInfo'
        or tostring(args[1])=='radio:onPlayerSyncBoomBox'
        or tostring(args[1])=='player:requestStreamData' then
        return
    end
    local ok,formatted=pcall(inspect,args)
    if not ok then formatted='<inspect error: '..tostring(formatted)..'>' end
    local row='['..tostring(resourceName)..' | '..tostring(file)..':'
        ..tostring(line)..'] '..tostring(functionName)..'(args: '
        ..tostring(#args)..'): '..tostring(formatted)
    dfEmit('event',row)
end

local function hiddenCall(name,original,...)
    if dfHideActive() then
        local info=debug and debug.getinfo and debug.getinfo(3,'Sl')
        emitEvent(R,name,true,info and info.short_src or '[injected]',
            info and info.currentline or 0,...)
    end
    return original(...)
end

local function wrappedServerEvent(...)
    return hiddenCall('triggerServerEvent',TS,...)
end

local function wrappedLatentEvent(...)
    return hiddenCall('triggerLatentServerEvent',TL,...)
end

local function finishScope(ok,...)
    Scope(false)
    if not ok then Raise((...),0) end
    return ...
end

local function scopedCall(original,...)
    Scope(true)
    return finishScope(PCall(original,...))
end

local function protect(original)
    return function(...)
        return scopedCall(original,...)
    end
end

local function injectedEnvironment()
    local env={}
    RawSet(env,'_G',env)
    RawSet(env,'triggerServerEvent',protect(wrappedServerEvent))
    RawSet(env,'triggerLatentServerEvent',protect(wrappedLatentEvent))
    SetMeta(env,{
        __index=function(_,key)
            local value=Global[key]
            if Kind(value)~='function' then return value end
            local wrapped=protect(value)
            RawSet(env,key,wrapped)
            return wrapped
        end,
        __newindex=function(_,key,value)
            RawSet(Global,key,value)
        end,
    })
    return env
end

local function update()
    local visible=dfMenuOpen()
    if visible~=O then
        O=visible
        toggleAllControls(not visible)
        guiSetInputMode(visible and 'no_binds' or 'allow_binds')
        showCursor(visible)
        if visible then setCursorAlpha(255) end
    end

    local code,target=dfTake()
    if not code then return end
    G=G+1
    hideFunctionCall(true)
    local ok,result=dfInject(target,code)
    if not ok then injectError('Inject error',result)
    else outputChatBox('[DarkFlame] Lua thread '..tostring(result)..' started.',96,255,128) end
    setTimer(restore,2000,1,G)
end

setTimer(update,100,0)
dfEmit('monitor','outgoing Lua packet monitor active',0)
)DFLUA";

using LuaCFunction = int(__cdecl*)(void*);
using LuaPCallFn = int(__cdecl*)(void*, int, int, int);
using LuaPushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
using LuaSetFieldFn = void(__cdecl*)(void*, int, const char*);
using LuaPushBooleanFn = void(__cdecl*)(void*, int);
using LuaToBooleanFn = int(__cdecl*)(void*, int);
using LuaToLStringFn = const char*(__cdecl*)(void*, int, std::size_t*);
using LuaPushStringFn = void(__cdecl*)(void*, const char*);
using LuaGetTopFn = int(__cdecl*)(void*);
using LuaGetFieldFn = void(__cdecl*)(void*, int, const char*);
using LuaSetTopFn = void(__cdecl*)(void*, int);
using LuaNewThreadFn = void*(__cdecl*)(void*);
using LuaLRefFn = int(__cdecl*)(void*, int);
using LuaLUnrefFn = void(__cdecl*)(void*, int, int);
using GetVirtualMachineFn = void*(__thiscall*)(void*, void*);
using IsNameAllowedFn = bool(__thiscall*)(void*, const char*, const void*, bool);
using CallHookFn = bool(__stdcall*)(const char*, const void*, const void*, bool);

std::atomic_bool g_ready{};
std::atomic_bool g_hideCalls{};
std::atomic_uint32_t g_scopeDepth{};
std::atomic_bool g_monitorReady{};
std::atomic_bool g_eventLogged{};
std::atomic_uint32_t g_monitorBlockedChecks{};
std::atomic_uint32_t g_monitorAllowedChecks{};
std::atomic_uint32_t g_controlChecks{};
std::atomic_uint32_t g_hideTransitions{};
std::atomic_uint32_t g_scopeTransitions{};
std::atomic_uint32_t g_bootstrapState{};
std::atomic<void*> g_bootstrapLua{};
std::atomic<void*> g_bootstrapMain{};
std::atomic<DWORD> g_reconnectCheckTick{};
std::mutex g_installMutex;
void* g_isNameAllowedTarget{};
void* g_callHookTarget{};
void* g_luaPCallTarget{};
void* g_triggerServerEventTarget{};
IsNameAllowedFn g_isNameAllowed{};
CallHookFn g_callHook{};
LuaPCallFn g_luaPCall{};
LuaCFunction g_triggerServerEvent{};
GetVirtualMachineFn g_getVirtualMachine{};
void** g_luaManagerSlot{};
LuaPushCClosureFn g_luaPushCClosure{};
LuaSetFieldFn g_luaSetField{};
LuaPushBooleanFn g_luaPushBoolean{};
LuaToBooleanFn g_luaToBoolean{};
LuaToLStringFn g_luaToLString{};
LuaPushStringFn g_luaPushString{};
LuaGetTopFn g_luaGetTop{};
LuaGetFieldFn g_luaGetField{};
LuaSetTopFn g_luaSetTop{};
LuaNewThreadFn g_luaNewThread{};
LuaLRefFn g_luaLRef{};
LuaLUnrefFn g_luaLUnref{};

struct LuaSession
{
    std::uintptr_t id;
    void* owner;
    void* main;
    void* thread;
    int reference;
    std::string resource;
};

std::vector<LuaSession> g_sessions;

bool HiddenActive()
{
    return g_hideCalls.load(std::memory_order_acquire)
        || g_scopeDepth.load(std::memory_order_acquire) != 0;
}

bool MonitorName(std::string_view name)
{
    return name == "triggerServerEvent"
        || name == "triggerLatentServerEvent";
}

bool ControlName(std::string_view name)
{
    return name == "addDebugHook" || name == "removeDebugHook";
}

std::wstring WideAscii(std::string_view text)
{
    return {text.begin(), text.end()};
}

std::string LuaText(void* lua, int index, std::size_t limit = 384,
    bool flatten = true)
{
    std::size_t length{};
    const char* text = g_luaToLString(lua, index, &length);
    if(!text)
        return "<value>";
    const std::size_t copied = std::min(length, limit);
    std::string output(text, copied);
    for(char& value : output)
    {
        if(flatten && (value == '\r' || value == '\n' || value == '\t'))
            value = ' ';
    }
    if(copied != length)
        output += "...";
    return output;
}

bool InjectIntoResource(std::string_view resource, std::string_view code,
    std::uintptr_t& id, std::string& error);
void DrainThreadRequests();
bool LuaIdentity(void* lua, void*& owner, void*& state);

void Register(void* lua, const char* name, LuaCFunction function)
{
    g_luaPushCClosure(lua, function, 0);
    g_luaSetField(lua, LuaGlobalsIndex, name);
}

int __cdecl HideFunctionCall(void* lua)
{
    const bool hidden = g_luaToBoolean(lua, 1) != 0;
    g_hideCalls.store(hidden, std::memory_order_release);
    if(g_hideTransitions.fetch_add(1, std::memory_order_relaxed) < 12)
        Log::Write(hidden ? L"[lua-bridge] hide filter enabled"
            : L"[lua-bridge] hide filter disabled");
    g_luaPushBoolean(lua, 1);
    return 1;
}

int __cdecl HideActive(void* lua)
{
    g_luaPushBoolean(lua, HiddenActive() ? 1 : 0);
    return 1;
}

int __cdecl TrapScope(void* lua)
{
    const bool enter = g_luaToBoolean(lua, 1) != 0;
    std::uint32_t depth{};
    if(enter)
    {
        depth = g_scopeDepth.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    else
    {
        depth = g_scopeDepth.load(std::memory_order_acquire);
        while(depth && !g_scopeDepth.compare_exchange_weak(depth, depth - 1,
            std::memory_order_acq_rel, std::memory_order_acquire))
        {
        }
        if(depth)
            --depth;
    }
    if(g_scopeTransitions.fetch_add(1, std::memory_order_relaxed) < 12)
    {
        Log::Write(std::wstring(L"[lua-bridge] trap scope ")
            + (enter ? L"enter, depth=" : L"leave, depth=")
            + std::to_wstring(depth));
    }
    g_luaPushBoolean(lua, 1);
    return 1;
}

int __cdecl BootstrapCode(void* lua)
{
    g_luaPushString(lua, LuaBootstrap);
    return 1;
}

int __cdecl TakeLuaCode(void* lua)
{
    DrainThreadRequests();
    std::string code;
    std::string resource;
    if(!GuiTakeLuaCode(code, resource))
        return 0;
    g_luaPushString(lua, code.c_str());
    g_luaPushString(lua, resource.c_str());
    return 2;
}

int __cdecl InjectResource(void* lua)
{
    const std::string resource = LuaText(lua, 1, 128);
    const std::string code = LuaText(lua, 2, 1024 * 1024, false);
    std::uintptr_t id{};
    std::string error;
    const bool injected = InjectIntoResource(resource, code, id, error);
    g_luaPushBoolean(lua, injected ? 1 : 0);
    if(injected)
    {
        char text[24]{};
        std::snprintf(text, sizeof(text), "0x%08lX",
            static_cast<unsigned long>(id));
        g_luaPushString(lua, text);
    }
    else
        g_luaPushString(lua, error.c_str());
    return 2;
}

int __cdecl MenuOpen(void* lua)
{
    g_luaPushBoolean(lua, GuiVisible() ? 1 : 0);
    return 1;
}

int __cdecl EmitEvent(void* lua)
{
    const int top = std::clamp(g_luaGetTop(lua), 0, 16);
    if(!top)
        return 0;

    if(LuaText(lua, 1) != "event")
        return 0;
    GuiAppendEvent(top >= 2 ? LuaText(lua, 2, 8192, false)
        : "[event] invalid callback payload");
    if(!g_eventLogged.exchange(true))
        Log::Write(L"[lua-bridge] first event callback reached");
    return 0;
}

void RegisterBridge(void* lua)
{
    Register(lua, "hideFunctionCall", &HideFunctionCall);
    Register(lua, "dfTrapScope", &TrapScope);
    Register(lua, "dfHideActive", &HideActive);
    Register(lua, "dfBootstrap", &BootstrapCode);
    Register(lua, "dfTake", &TakeLuaCode);
    Register(lua, "dfInject", &InjectResource);
    Register(lua, "dfMenuOpen", &MenuOpen);
    Register(lua, "dfEmit", &EmitEvent);
}

bool RunDirectBootstrap(void* lua)
{
    std::uint32_t expected{};
    if(!g_bootstrapState.compare_exchange_strong(expected, 1,
        std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return false;
    }

    const int top = g_luaGetTop(lua);
    g_scopeDepth.fetch_add(1, std::memory_order_acq_rel);

    RegisterBridge(lua);
    g_luaGetField(lua, LuaGlobalsIndex, "loadstring");
    g_luaPushString(lua, LuaBootstrap);
    int result = g_luaPCall(lua, 1, 1, 0);
    std::string error;
    if(result)
        error = "compile: " + LuaText(lua, -1);
    else
    {
        result = g_luaPCall(lua, 0, 0, 0);
        if(result)
            error = "runtime: " + LuaText(lua, -1);
    }

    g_luaSetTop(lua, top);
    g_scopeDepth.fetch_sub(1, std::memory_order_acq_rel);
    if(!result)
    {
        void* owner{};
        void* state = lua;
        LuaIdentity(lua, owner, state);
        g_bootstrapMain.store(owner, std::memory_order_release);
        g_bootstrapLua.store(state, std::memory_order_release);
        g_bootstrapState.store(2, std::memory_order_release);
        return true;
    }

    g_bootstrapLua.store(nullptr, std::memory_order_release);
    g_bootstrapMain.store(nullptr, std::memory_order_release);
    g_bootstrapState.store(0, std::memory_order_release);
    Log::Write(L"[lua-bridge] direct bootstrap failed: " + WideAscii(error));
    return true;
}

void LogMonitorDecision(const char* name, bool allowed,
    std::wstring_view reason)
{
    std::atomic_uint32_t& counter = allowed
        ? g_monitorAllowedChecks : g_monitorBlockedChecks;
    if(counter.fetch_add(1, std::memory_order_relaxed) >= 4)
        return;

    std::wstring message = L"[lua-bridge] IsNameAllowed ";
    message += WideAscii(name);
    message += allowed ? L": allow (" : L": block (";
    message += reason;
    message += L")";
    Log::Write(message);
}

void LogControlDecision(const char* name, bool allowed,
    std::wstring_view reason)
{
    if(g_controlChecks.fetch_add(1, std::memory_order_relaxed) >= 16)
        return;
    std::wstring message = L"[lua-bridge] IsNameAllowed ";
    message += WideAscii(name);
    message += allowed ? L": allow (" : L": block (";
    message += reason;
    message += L")";
    Log::Write(message);
}

struct LuaArgumentsView
{
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t capacity;
};

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
            sizeof(info)))
        {
            return false;
        }
        if(info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(base > std::numeric_limits<std::uintptr_t>::max() - info.RegionSize)
            return false;
        const std::uintptr_t regionEnd = base + info.RegionSize;
        if(regionEnd <= cursor)
            return false;
        cursor = std::min(regionEnd, finish);
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
    std::size_t limit = 768)
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
    const std::size_t copied = std::min<std::size_t>(length, limit);
    if(copied && !Readable(text, copied))
        return false;
    output.assign(text ? text : "", copied);
    return true;
}

struct LuaMainEntry
{
    void* object;
    void* state;
    std::string resource;
};

struct LuaMainNode
{
    LuaMainNode* next;
    LuaMainNode* previous;
    void* value;
};

bool LuaManager(void*& manager)
{
    manager = nullptr;
    return g_luaManagerSlot && ReadValue(g_luaManagerSlot, 0, manager) && manager;
}

bool ReadLuaMain(void* object, LuaMainEntry& entry)
{
    void* resource{};
    if(!object || !ReadValue(object, 24, entry.state) || !entry.state
        || !ReadValue(object, 72, resource) || !resource
        || !ReadSString(static_cast<const char*>(resource) + 8,
            entry.resource, 128))
    {
        return false;
    }
    entry.object = object;
    return !entry.resource.empty();
}

bool LuaIdentity(void* lua, void*& owner, void*& state)
{
    owner = nullptr;
    state = lua;
    if(!lua || !g_getVirtualMachine)
        return false;
    void* manager{};
    if(!LuaManager(manager))
        return false;
    LuaMainEntry entry{};
    if(!ReadLuaMain(g_getVirtualMachine(manager, lua), entry))
        return false;
    owner = entry.object;
    state = entry.state;
    return true;
}

bool LuaMains(std::vector<LuaMainEntry>& entries)
{
    entries.clear();
    void* manager{};
    LuaMainNode* head{};
    LuaMainNode value{};
    if(!LuaManager(manager) || !ReadValue(manager, 68, head) || !head
        || !ReadValue(head, 0, value))
    {
        return false;
    }

    LuaMainNode* node = value.next;
    for(std::size_t count = 0; node && node != head && count < 4096; ++count)
    {
        if(!ReadValue(node, 0, value))
            return false;
        LuaMainEntry entry{};
        if(ReadLuaMain(value.value, entry))
            entries.push_back(std::move(entry));
        node = value.next;
    }
    return node == head;
}

bool LuaStateAlive(void* state, void* owner = nullptr)
{
    std::vector<LuaMainEntry> entries;
    if(!LuaMains(entries))
        return false;
    return std::any_of(entries.begin(), entries.end(),
        [state, owner](const LuaMainEntry& entry)
    {
        return entry.state == state
            && (!owner || entry.object == owner);
    });
}

bool FindResourceState(std::string_view resource, LuaMainEntry& found)
{
    std::vector<LuaMainEntry> entries;
    if(!LuaMains(entries))
        return false;
    const auto match = std::find_if(entries.begin(), entries.end(),
        [resource](const LuaMainEntry& entry)
    {
        return entry.resource.size() == resource.size()
            && !_strnicmp(entry.resource.c_str(), resource.data(), resource.size());
    });
    if(match == entries.end())
        return false;
    found = *match;
    return true;
}

std::string CurrentLuaResource(void* lua)
{
    if(!lua || !g_luaManagerSlot || !g_getVirtualMachine)
        return "unknown";
    void* manager{};
    if(!ReadValue(g_luaManagerSlot, 0, manager) || !manager)
        return "unknown";
    void* luaMain = g_getVirtualMachine(manager, lua);
    LuaMainEntry entry{};
    return ReadLuaMain(luaMain, entry) ? entry.resource : "unknown";
}

std::string LuaLiteral(std::string_view value)
{
    std::string output{"\""};
    output.reserve(value.size() + 16);
    for(const unsigned char character : value)
    {
        switch(character)
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if(character >= 0x20 && character < 0x7F)
                output.push_back(static_cast<char>(character));
            else
            {
                char escaped[5]{};
                std::snprintf(escaped, sizeof(escaped), "\\%03u", character);
                output += escaped;
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::string ThreadKey(std::uintptr_t id)
{
    char key[24]{};
    std::snprintf(key, sizeof(key), "0x%08lX", static_cast<unsigned long>(id));
    return key;
}

std::string ManagedChunk(std::uintptr_t id, std::string_view userCode)
{
    const std::string key = LuaLiteral(ThreadKey(id));
    std::string code;
    code.reserve(userCode.size() + 2300);
    code += "local B=_G local K=" + key
        + " local S=rawget(B,'__darkFlameThreads') "
        "if not S then S={} rawset(B,'__darkFlameThreads',S) end "
        "local C={} local function T(f) C[#C+1]=f end local E={} E._G=E "
        "E.onUnload=function(f) if B.type(f)=='function' then T(f) return true end return false end "
        "E.setTimer=function(f,d,n,...) local t=B.setTimer(f,d,n,...) "
        "if t then T(function() if B.isTimer(t) then B.killTimer(t) end end) end return t end "
        "E.addEventHandler=function(n,e,f,...) local o=B.addEventHandler(n,e,f,...) "
        "if o then T(function() B.removeEventHandler(n,e,f) end) end return o end "
        "E.bindKey=function(k,s,f,...) local o=B.bindKey(k,s,f,...) "
        "if o then T(function() B.unbindKey(k,s,f) end) end return o end "
        "E.addCommandHandler=function(n,f,...) local o=B.addCommandHandler(n,f,...) "
        "if o then T(function() B.removeCommandHandler(n,f) end) end return o end "
        "E.createElement=function(...) local e=B.createElement(...) "
        "if e then T(function() if B.isElement(e) then B.destroyElement(e) end end) end return e end "
        "B.setmetatable(E,{__index=B}) "
        "local F,X=B.loadstring(" + LuaLiteral(userCode) + ") "
        "if not F then B.error(X,0) end B.setfenv(F,E) S[K]={cleanup=C} "
        "local O,R=B.pcall(F) if not O then B.error(R,0) end return R";
    return code;
}

std::string CleanupChunk(std::uintptr_t id)
{
    return "local S=rawget(_G,'__darkFlameThreads') local T=S and S["
        + LuaLiteral(ThreadKey(id))
        + "] if T then for I=#T.cleanup,1,-1 do pcall(T.cleanup[I]) end "
        "S[" + LuaLiteral(ThreadKey(id)) + "]=nil end";
}

bool RunLuaChunk(void* lua, std::string_view code, std::string& error)
{
    const int top = g_luaGetTop(lua);
    g_luaGetField(lua, LuaGlobalsIndex, "loadstring");
    const std::string source(code);
    g_luaPushString(lua, source.c_str());
    int result = g_luaPCall(lua, 1, 1, 0);
    if(!result)
        result = g_luaPCall(lua, 0, 0, 0);
    if(result)
        error = LuaText(lua, -1, 2048, false);
    g_luaSetTop(lua, top);
    return result == 0;
}

std::string Timestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char output[24]{};
    std::snprintf(output, sizeof(output), "%02u:%02u:%02u.%03u",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return output;
}

void RemoveSession(std::vector<LuaSession>::iterator session, bool release)
{
    const std::uintptr_t id = session->id;
    if(release)
    {
        std::string ignored;
        g_scopeDepth.fetch_add(1, std::memory_order_acq_rel);
        RunLuaChunk(session->main, CleanupChunk(id), ignored);
        g_luaLUnref(session->main, LuaRegistryIndex, session->reference);
        g_scopeDepth.fetch_sub(1, std::memory_order_acq_rel);
    }
    g_sessions.erase(session);
    GuiRemoveLuaThread(id);
}

void PruneDeadSessions()
{
    if(g_sessions.empty())
        return;
    std::vector<LuaMainEntry> entries;
    if(!LuaMains(entries))
        return;
    for(auto session = g_sessions.begin(); session != g_sessions.end();)
    {
        const bool alive = std::any_of(entries.begin(), entries.end(),
            [&](const LuaMainEntry& entry)
        {
            return entry.state == session->main
                && entry.object == session->owner;
        });
        if(alive)
            ++session;
        else
        {
            const std::uintptr_t id = session->id;
            session = g_sessions.erase(session);
            GuiRemoveLuaThread(id);
        }
    }
}

bool InjectIntoResource(std::string_view resource, std::string_view code,
    std::uintptr_t& id, std::string& error)
{
    PruneDeadSessions();
    LuaMainEntry target{};
    if(resource.empty() || !FindResourceState(resource, target))
    {
        error = "resource VM not found: " + std::string(resource);
        return false;
    }

    const int mainTop = g_luaGetTop(target.state);
    void* thread = g_luaNewThread(target.state);
    if(!thread)
    {
        g_luaSetTop(target.state, mainTop);
        error = "lua_newthread failed";
        return false;
    }
    const int reference = g_luaLRef(target.state, LuaRegistryIndex);
    g_luaSetTop(target.state, mainTop);
    id = reinterpret_cast<std::uintptr_t>(thread);

    g_scopeDepth.fetch_add(1, std::memory_order_acq_rel);
    const bool executed = RunLuaChunk(thread, ManagedChunk(id, code), error);
    g_scopeDepth.fetch_sub(1, std::memory_order_acq_rel);
    if(!executed)
    {
        std::string ignored;
        RunLuaChunk(target.state, CleanupChunk(id), ignored);
        g_luaLUnref(target.state, LuaRegistryIndex, reference);
        return false;
    }

    g_sessions.push_back({id, target.object, target.state, thread, reference,
        target.resource});
    GuiAddLuaThread({id, Timestamp(), target.resource});
    Log::Write(L"[lua-bridge] Lua thread started: " + WideAscii(ThreadKey(id))
        + L" resource=" + WideAscii(target.resource));
    return true;
}

void DrainThreadRequests()
{
    PruneDeadSessions();
    std::uintptr_t id{};
    while(GuiTakeUnloadThread(id))
    {
        const auto session = std::find_if(g_sessions.begin(), g_sessions.end(),
            [id](const LuaSession& item) { return item.id == id; });
        if(session == g_sessions.end())
            continue;
        RemoveSession(session, LuaStateAlive(session->main, session->owner));
        Log::Write(L"[lua-bridge] Lua thread unloaded: " + WideAscii(ThreadKey(id)));
    }
}

void ResetForReconnect()
{
    if(g_bootstrapState.load(std::memory_order_acquire) != 2)
        return;
    void* previous = g_bootstrapLua.load(std::memory_order_acquire);
    void* owner = g_bootstrapMain.load(std::memory_order_acquire);
    if(!previous || !owner)
        return;
    const DWORD now = GetTickCount();
    DWORD checked = g_reconnectCheckTick.load(std::memory_order_relaxed);
    if(now - checked < 1000 || !g_reconnectCheckTick.compare_exchange_strong(
        checked, now, std::memory_order_relaxed))
    {
        return;
    }
    if(LuaStateAlive(previous, owner))
        return;

    std::uint32_t expected = 2;
    if(!g_bootstrapState.compare_exchange_strong(expected, 0,
        std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return;
    }
    g_bootstrapLua.store(nullptr, std::memory_order_release);
    g_bootstrapMain.store(nullptr, std::memory_order_release);
    g_reconnectCheckTick.store(0, std::memory_order_release);
    g_sessions.clear();
    GuiClearLuaThreads();
    g_hideCalls.store(false, std::memory_order_release);
    g_scopeDepth.store(0, std::memory_order_release);
}

struct LuaFunctionNode
{
    LuaFunctionNode* left;
    LuaFunctionNode* parent;
    LuaFunctionNode* right;
    std::uint8_t color;
    std::uint8_t nil;
    std::uint16_t padding;
    LuaCFunction function;
    void* descriptor;
};

static_assert(sizeof(LuaFunctionNode) == 24);

LuaCFunction FindRegisteredLuaFunction(std::uintptr_t getFunction,
    std::string_view wanted)
{
    std::uint8_t mapLoadOpcode{};
    std::uintptr_t mapAddress{};
    if(!ReadValue(reinterpret_cast<const void*>(getFunction), 0x1D,
        mapLoadOpcode) || mapLoadOpcode != 0xB9
        || !ReadValue(reinterpret_cast<const void*>(getFunction), 0x1E,
            mapAddress))
    {
        return nullptr;
    }

    LuaFunctionNode* head{};
    LuaFunctionNode headValue{};
    if(!ReadValue(reinterpret_cast<const void*>(mapAddress), 0, head)
        || !head || !ReadValue(head, 0, headValue))
    {
        return nullptr;
    }

    std::vector<LuaFunctionNode*> pending{headValue.parent};
    for(std::size_t visited = 0; !pending.empty() && visited < 4096;
        ++visited)
    {
        LuaFunctionNode* node = pending.back();
        pending.pop_back();
        LuaFunctionNode value{};
        if(!node || node == head || !ReadValue(node, 0, value) || value.nil)
            continue;

        std::string name;
        if(value.descriptor && ReadSString(
            static_cast<const char*>(value.descriptor) + 8, name, 96)
            && name == wanted)
        {
            return value.function;
        }
        if(value.left && value.left != head)
            pending.push_back(value.left);
        if(value.right && value.right != head)
            pending.push_back(value.right);
    }
    return nullptr;
}

int __cdecl HookTriggerServerEvent(void* lua)
{
    SetNetcLuaCallContext(CurrentLuaResource(lua));
    const int result = g_triggerServerEvent(lua);
    ClearNetcLuaCallContext();
    return result;
}

bool ReadArguments(const void* arguments, LuaArgumentsView& view,
    std::size_t& count)
{
    if(!ReadValue(arguments, 0, view) || view.end < view.begin
        || view.capacity < view.end || (view.end - view.begin) % sizeof(void*))
    {
        return false;
    }
    count = (view.end - view.begin) / sizeof(void*);
    return count <= 256 && (!count || Readable(
        reinterpret_cast<const void*>(view.begin), count * sizeof(void*)));
}

const void* ArgumentAt(const LuaArgumentsView& view, std::size_t index)
{
    const void* argument{};
    if(!ReadValue(reinterpret_cast<const void*>(view.begin),
        index * sizeof(void*), argument))
    {
        return nullptr;
    }
    return argument;
}

bool ArgumentString(const void* argument, std::string& output,
    std::size_t limit = 512)
{
    std::uint32_t length{};
    std::uint32_t capacity{};
    if(!ReadValue(argument, 40, length) || !ReadValue(argument, 44, capacity)
        || length > capacity || length > 1024 * 1024)
    {
        return false;
    }
    const char* text{};
    if(capacity <= 15)
        text = reinterpret_cast<const char*>(argument) + 24;
    else if(!ReadValue(argument, 24, text))
        return false;
    const std::size_t copied = std::min<std::size_t>(length, limit);
    if(copied && !Readable(text, copied))
        return false;
    output.assign(text ? text : "", copied);
    if(copied != length)
        output += "...";
    return true;
}

std::string Escape(std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for(const unsigned char character : value)
    {
        switch(character)
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\r': output += "\\r"; break;
        case '\n': output += "\\n"; break;
        case '\t': output += "\\t"; break;
        default:
            if(character >= 0x20)
                output.push_back(static_cast<char>(character));
            else
            {
                char escaped[5]{};
                std::snprintf(escaped, sizeof(escaped), "\\x%02X", character);
                output += escaped;
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::string Hex(std::uintptr_t value)
{
    char output[24]{};
    std::snprintf(output, sizeof(output), "0x%08lX",
        static_cast<unsigned long>(value));
    return output;
}

std::string FormatArgument(const void* argument, unsigned depth = 0)
{
    int type{-1};
    if(!ReadValue(argument, 0, type))
        return "<invalid>";
    switch(type)
    {
    case 0:
        return "nil";
    case 1:
    {
        bool value{};
        return ReadValue(argument, 8, value)
            ? (value ? "true" : "false") : "<invalid-bool>";
    }
    case 2:
    case 7:
    {
        std::uintptr_t value{};
        return ReadValue(argument, 48, value)
            ? (type == 7 ? "userdata(" : "lightuserdata(") + Hex(value) + ")"
            : "<invalid-userdata>";
    }
    case 3:
    {
        double value{};
        if(!ReadValue(argument, 16, value))
            return "<invalid-number>";
        char output[64]{};
        const auto result = std::to_chars(output, output + sizeof(output), value,
            std::chars_format::general, 12);
        return result.ec == std::errc{} ? std::string(output, result.ptr)
            : "<number>";
    }
    case 4:
    case 10:
    {
        std::string value;
        return ArgumentString(argument, value) ? Escape(value) : "<invalid-string>";
    }
    case 5:
    case 9:
    {
        const void* table{};
        if(depth >= 2 || !ReadValue(argument, 52, table))
            return depth >= 2 ? "{...}" : "<invalid-table>";
        LuaArgumentsView view{};
        std::size_t count{};
        if(!ReadArguments(table, view, count))
            return "<invalid-table>";
        std::string output = "{";
        const std::size_t displayed = std::min<std::size_t>(count, 16);
        for(std::size_t index = 0; index < displayed; ++index)
        {
            if(index)
                output += ", ";
            output += FormatArgument(ArgumentAt(view, index), depth + 1);
        }
        if(displayed != count)
            output += ", ...";
        output += "}";
        return output;
    }
    case 6:
        return "<function>";
    case 8:
        return "<thread>";
    default:
        return "<type:" + std::to_string(type) + ">";
    }
}

bool NativeEventRow(const char* name, const void* arguments, std::string& row)
{
    LuaArgumentsView view{};
    std::size_t count{};
    if(!ReadArguments(arguments, view, count) || count < 5)
        return false;

    std::string file;
    if(!ArgumentString(ArgumentAt(view, 3), file, 768))
        file = "?";
    double line{};
    if(!ReadValue(ArgumentAt(view, 4), 16, line))
        line = 0;

    std::string eventName;
    if(count > 5 && ArgumentString(ArgumentAt(view, 5), eventName)
        && (eventName == "inventory:getPlayersObjectInfo"
            || eventName == "radio:onPlayerSyncBoomBox"
            || eventName == "player:requestStreamData"))
    {
        return false;
    }

    row = "[native ";
    row += FormatArgument(ArgumentAt(view, 0));
    row += " | " + file + ":" + std::to_string(static_cast<int>(line)) + "] ";
    row += name;
    row += "(args: " + std::to_string(count - 5) + "): ";
    for(std::size_t index = 5; index < count; ++index)
    {
        if(index != 5)
            row += ", ";
        row += FormatArgument(ArgumentAt(view, index));
        if(row.size() > 8192)
        {
            row.resize(8192);
            row += "...";
            break;
        }
    }
    return true;
}

bool __stdcall HookCallHook(const char* name, const void* eventHookList,
    const void* arguments, bool explicitlyAllowed)
{
    try
    {
        if(name && MonitorName(name) && !HiddenActive())
        {
            std::string row;
            if(NativeEventRow(name, arguments, row))
            {
                GuiAppendEvent(row);
                if(!g_eventLogged.exchange(true))
                    Log::Write(L"[lua-bridge] first native CallHook event reached");
            }
        }
    }
    catch(...)
    {
        Log::Write(L"[lua-bridge] native CallHook formatter failed");
    }
    return g_callHook(name, eventHookList, arguments, explicitlyAllowed);
}

bool __fastcall HookIsNameAllowed(void* self, void*, const char* name,
    const void* eventHookList, bool explicitlyAllowed)
{
    const bool hidden = HiddenActive();
    if(hidden)
    {
        if(name && MonitorName(name))
            LogMonitorDecision(name, false, L"protected execution scope");
        if(name && ControlName(name))
            LogControlDecision(name, false, L"protected execution scope");
        return false;
    }
    if(!hidden && g_monitorReady.load(std::memory_order_acquire)
        && name && MonitorName(name))
    {
        LogMonitorDecision(name, true, L"DarkFlame monitor ready");
        return true;
    }
    const bool allowed = g_isNameAllowed(self, name, eventHookList,
        explicitlyAllowed);
    if(name && ControlName(name))
        LogControlDecision(name, allowed, L"original decision");
    return allowed;
}

int __cdecl HookLuaPCall(void* lua, int argumentCount, int resultCount,
    int errorFunction)
{
    if(g_ready.load(std::memory_order_acquire))
    {
        ResetForReconnect();
        RunDirectBootstrap(lua);
    }
    return g_luaPCall(lua, argumentCount, resultCount, errorFunction);
}

bool InstallHook(void* target, void* detour, void** original)
{
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if(created != MH_OK)
        return false;
    if(MH_EnableHook(target) == MH_OK)
        return true;
    MH_RemoveHook(target);
    *original = nullptr;
    return false;
}

void RemoveHooks()
{
    if(g_luaPCallTarget)
    {
        MH_DisableHook(g_luaPCallTarget);
        MH_RemoveHook(g_luaPCallTarget);
    }
    if(g_isNameAllowedTarget)
    {
        MH_DisableHook(g_isNameAllowedTarget);
        MH_RemoveHook(g_isNameAllowedTarget);
    }
    if(g_callHookTarget)
    {
        MH_DisableHook(g_callHookTarget);
        MH_RemoveHook(g_callHookTarget);
    }
    if(g_triggerServerEventTarget)
    {
        MH_DisableHook(g_triggerServerEventTarget);
        MH_RemoveHook(g_triggerServerEventTarget);
    }
    g_luaPCallTarget = nullptr;
    g_isNameAllowedTarget = nullptr;
    g_callHookTarget = nullptr;
    g_triggerServerEventTarget = nullptr;
    g_luaPCall = nullptr;
    g_luaGetField = nullptr;
    g_luaSetTop = nullptr;
    g_luaNewThread = nullptr;
    g_luaLRef = nullptr;
    g_luaLUnref = nullptr;
    g_isNameAllowed = nullptr;
    g_callHook = nullptr;
    g_triggerServerEvent = nullptr;
    g_getVirtualMachine = nullptr;
    g_luaManagerSlot = nullptr;
    ClearNetcLuaCallContext();
    g_hideCalls.store(false, std::memory_order_release);
    g_scopeDepth.store(0, std::memory_order_release);
    g_monitorReady.store(false, std::memory_order_release);
    g_bootstrapState.store(0, std::memory_order_release);
    g_bootstrapLua.store(nullptr, std::memory_order_release);
    g_bootstrapMain.store(nullptr, std::memory_order_release);
    g_reconnectCheckTick.store(0, std::memory_order_release);
    g_sessions.clear();
    GuiClearLuaThreads();
}

std::uintptr_t Find(const SignatureScanner& scanner, std::string_view pattern,
    const wchar_t* name)
{
    const std::uintptr_t address = scanner.Find(pattern);
    Log::Scan(name, address ? L"found" : L"not_found", address);
    return address;
}
}

bool InstallLuaBridge(HMODULE client)
{
    if(g_ready.load(std::memory_order_acquire))
        return true;
    std::scoped_lock lock(g_installMutex);
    if(g_ready.load(std::memory_order_relaxed))
        return true;

    const SignatureScanner scanner(client);
    const std::uintptr_t isNameAllowed = Find(scanner, IsNameAllowedPattern,
        L"CDebugHookManager::IsNameAllowed");
    const std::uintptr_t callHook = Find(scanner, CallHookPattern,
        L"CDebugHookManager::CallHook");
    const std::uintptr_t luaPCall = Find(scanner, LuaPCallPattern, L"lua_pcall");
    const std::uintptr_t pushCClosure = Find(scanner, LuaPushCClosurePattern,
        L"lua_pushcclosure");
    const std::uintptr_t setField = Find(scanner, LuaSetFieldPattern,
        L"lua_setfield");
    const std::uintptr_t pushBoolean = Find(scanner, LuaPushBooleanPattern,
        L"lua_pushboolean");
    const std::uintptr_t toBoolean = Find(scanner, LuaToBooleanPattern,
        L"lua_toboolean");
    const std::uintptr_t toLString = Find(scanner, LuaToLStringPattern,
        L"lua_tolstring");
    const std::uintptr_t pushString = Find(scanner, LuaPushStringPattern,
        L"lua_pushstring");
    const std::uintptr_t getTop = Find(scanner, LuaGetTopPattern, L"lua_gettop");
    const std::uintptr_t getField = Find(scanner, LuaGetFieldPattern,
        L"lua_getfield");
    const std::uintptr_t setTop = Find(scanner, LuaSetTopPattern, L"lua_settop");
    const std::uintptr_t newThread = Find(scanner, LuaNewThreadPattern,
        L"lua_newthread");
    const std::uintptr_t luaLRef = Find(scanner, LuaLRefPattern, L"luaL_ref");
    const std::uintptr_t luaLUnref = Find(scanner, LuaLUnrefPattern,
        L"luaL_unref");
    const std::uintptr_t getLuaFunction = Find(scanner,
        LuaFunctionRegistryPattern,
        L"CLuaCFunctions::GetFunction");
    const std::uintptr_t getVirtualMachine = Find(scanner,
        GetVirtualMachinePattern, L"CLuaManager::GetVirtualMachine");
    const std::uintptr_t luaManagerLoad = Find(scanner, LuaManagerLoadPattern,
        L"CLuaDefs::m_pLuaManager load");
    if(!isNameAllowed || !callHook || !luaPCall || !pushCClosure || !setField
        || !pushBoolean || !toBoolean || !toLString || !pushString || !getTop
        || !getField || !setTop || !newThread || !luaLRef || !luaLUnref
        || !getLuaFunction || !getVirtualMachine || !luaManagerLoad)
    {
        Log::Write(L"[lua-bridge] signature resolution failed");
        return false;
    }

    g_luaPushCClosure = reinterpret_cast<LuaPushCClosureFn>(pushCClosure);
    g_luaSetField = reinterpret_cast<LuaSetFieldFn>(setField);
    g_luaPushBoolean = reinterpret_cast<LuaPushBooleanFn>(pushBoolean);
    g_luaToBoolean = reinterpret_cast<LuaToBooleanFn>(toBoolean);
    g_luaToLString = reinterpret_cast<LuaToLStringFn>(toLString);
    g_luaPushString = reinterpret_cast<LuaPushStringFn>(pushString);
    g_luaGetTop = reinterpret_cast<LuaGetTopFn>(getTop);
    g_luaGetField = reinterpret_cast<LuaGetFieldFn>(getField);
    g_luaSetTop = reinterpret_cast<LuaSetTopFn>(setTop);
    g_luaNewThread = reinterpret_cast<LuaNewThreadFn>(newThread);
    g_luaLRef = reinterpret_cast<LuaLRefFn>(luaLRef);
    g_luaLUnref = reinterpret_cast<LuaLUnrefFn>(luaLUnref);
    g_getVirtualMachine = reinterpret_cast<GetVirtualMachineFn>(getVirtualMachine);
    g_luaManagerSlot = *reinterpret_cast<void***>(luaManagerLoad + 2);
    g_isNameAllowedTarget = reinterpret_cast<void*>(isNameAllowed);
    g_callHookTarget = reinterpret_cast<void*>(callHook);
    g_luaPCallTarget = reinterpret_cast<void*>(luaPCall);
    for(DWORD waited = 0; waited <= 5000 && !g_triggerServerEventTarget;
        waited += 10)
    {
        g_triggerServerEventTarget = reinterpret_cast<void*>(
            FindRegisteredLuaFunction(getLuaFunction, "triggerServerEvent"));
        if(!g_triggerServerEventTarget && waited != 5000)
            Sleep(10);
    }
    Log::Scan(L"CLuaFunctionDefs::TriggerServerEvent",
        g_triggerServerEventTarget ? L"resolved" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_triggerServerEventTarget));
    if(!g_triggerServerEventTarget)
    {
        Log::Write(L"[lua-bridge] triggerServerEvent registry lookup failed");
        return false;
    }

    InitializeNetcClientApi(client);

    if(!InstallHook(g_callHookTarget, reinterpret_cast<void*>(&HookCallHook),
        reinterpret_cast<void**>(&g_callHook))
        || !InstallHook(g_isNameAllowedTarget,
        reinterpret_cast<void*>(&HookIsNameAllowed),
        reinterpret_cast<void**>(&g_isNameAllowed))
        || !InstallHook(g_luaPCallTarget, reinterpret_cast<void*>(&HookLuaPCall),
            reinterpret_cast<void**>(&g_luaPCall))
        || !InstallHook(g_triggerServerEventTarget,
            reinterpret_cast<void*>(&HookTriggerServerEvent),
            reinterpret_cast<void**>(&g_triggerServerEvent)))
    {
        RemoveHooks();
        Log::Write(L"[lua-bridge] hook installation failed");
        return false;
    }

    g_monitorReady.store(true, std::memory_order_release);
    g_bootstrapState.store(0, std::memory_order_release);
    g_bootstrapLua.store(nullptr, std::memory_order_release);
    g_bootstrapMain.store(nullptr, std::memory_order_release);
    g_reconnectCheckTick.store(0, std::memory_order_release);
    g_ready.store(true, std::memory_order_release);
    Log::Write(L"[lua-bridge] targeted packet context, native monitor, and Lua bridge ready");
    return true;
}
