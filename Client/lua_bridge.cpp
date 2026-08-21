#include "lua_bridge.h"

#include "gui.h"
#include "logger.h"
#include "netc_hooks.h"
#include "signature_scanner.h"

#include <MinHook.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "winmm.lib")

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
constexpr std::string_view AddDebugHookPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 81 EC ? ? ? ? A1 ? ? ? ? "
    "33 C5 89 45 ? 53 56 57 50 8D 45 ? 64 A3 ? ? ? ? 8B 45 ? 8B 55";
constexpr std::string_view RemoveDebugHookPattern =
    "55 8B EC A1 ? ? ? ? 53 56 57 8B B0";
constexpr std::string_view LuaMToRefPattern =
    "55 8B EC 83 EC ? A1 ? ? ? ? 56 8B 75";
constexpr std::string_view LuaFunctionRefDtorPattern =
    "55 8B EC 6A ? 68 ? ? ? ? 64 A1 ? ? ? ? 50 56 A1 ? ? ? ? 33 C5 50 "
    "8D 45 ? 64 A3 ? ? ? ? 8B F1 FF 76 ? FF 76";
constexpr std::string_view LuaTypePattern =
    "55 8B EC FF 75 ? FF 75 ? E8 ? ? ? ? 83 C4 ? 3D ? ? ? ? 75";
constexpr std::string_view LuaPushNilPattern =
    "55 8B EC 8B 55 ? 8B 42 ? C7 40";
constexpr std::string_view LuaNextPattern =
    "55 8B EC 56 FF 75 ? 8B 75 ? 56 E8 ? ? ? ? 83 C4 ? 83 78 ? ? 74 ? "
    "C7 05 ? ? ? ? ? ? ? ? 8B 4E ? 83 E9 ? 51 FF 30 56";
constexpr std::string_view LuaPushValuePattern =
    "55 8B EC 56 57 FF 75 ? 8B 7D";
constexpr std::string_view ClientGameDebugHookAccessPattern =
    "A1 ? ? ? ? 8D 8D ? ? ? ? 51 8D 8D ? ? ? ? 51 FF B5 ? ? ? ? "
    "8B 88 ? ? ? ? E8 ? ? ? ?";

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
using LuaTypeFn = int(__cdecl*)(void*, int);
using LuaPushNilFn = void(__cdecl*)(void*);
using LuaNextFn = int(__cdecl*)(void*, int);
using LuaPushValueFn = void(__cdecl*)(void*, int);
using GetVirtualMachineFn = void*(__thiscall*)(void*, void*);
using IsNameAllowedFn = bool(__thiscall*)(void*, const char*, const void*, bool);
using CallHookFn = bool(__stdcall*)(const char*, const void*, const void*, bool);
using AddDebugHookFn = bool(__thiscall*)(void*, int, const void*, const void*);
using RemoveDebugHookFn = bool(__thiscall*)(void*, int, const void*);
using LuaMToRefFn = void*(__cdecl*)(void*, void*, int);
using LuaFunctionRefDtorFn = void(__thiscall*)(void*);

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
std::atomic_uint32_t g_tramBootstrapState{};
std::atomic<DWORD> g_tramRetryTick{};
std::atomic_bool g_tramResourceSeen{};
std::atomic<DWORD> g_tramResourceThread{};
std::atomic_bool g_tramFallbackLogged{};
std::atomic_bool g_tramUiReadyLogged{};
void* g_tramOwner{};
void* g_tramMain{};
std::wstring g_tramScriptPath;
std::mutex g_installMutex;
void* g_isNameAllowedTarget{};
void* g_callHookTarget{};
void* g_luaPCallTarget{};
void* g_triggerServerEventTarget{};
IsNameAllowedFn g_isNameAllowed{};
CallHookFn g_callHook{};
LuaPCallFn g_luaPCall{};
LuaCFunction g_triggerServerEvent{};
LuaCFunction g_triggerEvent{};
LuaCFunction g_addEvent{};
LuaCFunction g_addEventHandler{};
LuaCFunction g_removeEventHandler{};
GetVirtualMachineFn g_getVirtualMachine{};
void** g_luaManagerSlot{};
void** g_clientGameSlot{};
std::size_t g_debugHookManagerOffset{};
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
LuaTypeFn g_luaType{};
LuaPushNilFn g_luaPushNil{};
LuaNextFn g_luaNext{};
LuaPushValueFn g_luaPushValue{};
AddDebugHookFn g_addDebugHook{};
RemoveDebugHookFn g_removeDebugHook{};
LuaMToRefFn g_luaMToRef{};
LuaFunctionRefDtorFn g_luaFunctionRefDtor{};

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

struct EventCatcher
{
    std::uint32_t id;
    void* owner;
    void* main;
    std::string name;
    std::string key;
    int skip;
};

std::mutex g_catcherMutex;
std::vector<EventCatcher> g_eventCatchers;
std::atomic_uint32_t g_nextCatcher{1};
thread_local bool g_dispatchingCatchers{};

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
std::string CurrentLuaResource(void* lua);
int __cdecl DirectTriggerServerEvent(void* lua);
int __cdecl DirectTriggerEvent(void* lua);
int __cdecl DirectAddEvent(void* lua);
int __cdecl DirectAddEventHandler(void* lua);
int __cdecl DirectRemoveEventHandler(void* lua);
int __cdecl DirectAddDebugHook(void* lua);
int __cdecl DirectRemoveDebugHook(void* lua);
int __cdecl DirectEmulateKey(void* lua);
int __cdecl DirectPlayAlertSignal(void* lua);
int __cdecl DirectCatchServerEvent(void* lua);
int __cdecl DirectRemoveEventCatcher(void* lua);
int __cdecl DirectTramTakeCommand(void* lua);
int __cdecl DirectTramUpdate(void* lua);
void DispatchEventCatchers(void* lua);

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

int __cdecl DirectTramTakeCommand(void* lua)
{
    std::string command;
    if(!GuiTakeTramCommand(command))
        return 0;
    g_luaPushString(lua, command.c_str());
    Log::Write(L"[trambot] Lua consumed GUI command: " + WideAscii(command));
    return 1;
}

int __cdecl DirectTramUpdate(void* lua)
{
    if(g_luaGetTop(lua) < 2)
    {
        g_luaPushBoolean(lua, 0);
        return 1;
    }
    const std::string key = LuaText(lua, 1, 64, false);
    const std::string value = LuaText(lua, 2, 512, false);
    GuiUpdateTramState(key, value);
    if(key == "loaded")
    {
        const bool loaded = value == "1" || value == "true";
        if(loaded && !g_tramUiReadyLogged.exchange(true))
            Log::Write(L"[trambot] Lua GUI bridge online");
        else if(!loaded)
            g_tramUiReadyLogged.store(false, std::memory_order_release);
    }
    g_luaPushBoolean(lua, 1);
    return 1;
}

void RegisterDirectAliases(void* lua)
{
    Register(lua, "dfTriggerServerEvent", &DirectTriggerServerEvent);
    Register(lua, "dfTriggerEvent", &DirectTriggerEvent);
    Register(lua, "dfAddEvent", &DirectAddEvent);
    Register(lua, "dfAddEventHandler", &DirectAddEventHandler);
    Register(lua, "dfRemoveEventHandler", &DirectRemoveEventHandler);
    Register(lua, "dfAddDebugHook", &DirectAddDebugHook);
    Register(lua, "dfRemoveDebugHook", &DirectRemoveDebugHook);
    Register(lua, "dfEmulateKey", &DirectEmulateKey);
    Register(lua, "dfPlayAlertSignal", &DirectPlayAlertSignal);
    Register(lua, "dfCatchServerEvent", &DirectCatchServerEvent);
    Register(lua, "dfRemoveEventCatcher", &DirectRemoveEventCatcher);
    Register(lua, "dfTramTakeCommand", &DirectTramTakeCommand);
    Register(lua, "dfTramUpdate", &DirectTramUpdate);
    Register(lua, "dfMenuOpen", &MenuOpen);
}

void RegisterBridge(void* lua)
{
    RegisterDirectAliases(lua);
    Register(lua, "hideFunctionCall", &HideFunctionCall);
    Register(lua, "dfTrapScope", &TrapScope);
    Register(lua, "dfHideActive", &HideActive);
    Register(lua, "dfBootstrap", &BootstrapCode);
    Register(lua, "dfTake", &TakeLuaCode);
    Register(lua, "dfInject", &InjectResource);
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

struct LuaFunctionRefStorage
{
    alignas(void*) std::byte bytes[24];
};

static_assert(sizeof(LuaFunctionRefStorage) == 24);
static_assert(sizeof(std::string) == 24);

template<class T>
bool ReadValue(const void* base, std::size_t offset, T& value);

bool DebugHookType(void* lua, int& hookType)
{
    if(!g_luaType || g_luaType(lua, 1) != 4)
        return false;
    const std::string name = LuaText(lua, 1, 32);
    constexpr std::string_view names[] = {
        "preEvent", "postEvent", "preFunction", "postFunction",
        "preEventFunction", "postEventFunction"
    };
    const auto found = std::find(std::begin(names), std::end(names), name);
    if(found == std::end(names))
        return false;
    hookType = static_cast<int>(found - std::begin(names));
    return true;
}

bool AllowedDebugHookNames(void* lua, std::vector<std::string>& names)
{
    const int top = g_luaGetTop(lua);
    if(top < 3 || g_luaType(lua, 3) == 0)
        return true;
    if(g_luaType(lua, 3) != 5)
        return false;

    g_luaPushNil(lua);
    while(g_luaNext(lua, 3))
    {
        if(names.size() >= 1024 || g_luaType(lua, -1) != 4)
        {
            g_luaSetTop(lua, top);
            return false;
        }
        std::size_t length{};
        const char* value = g_luaToLString(lua, -1, &length);
        if(!value || length > 1024)
        {
            g_luaSetTop(lua, top);
            return false;
        }
        names.emplace_back(value, length);
        g_luaSetTop(lua, -2);
    }
    return true;
}

void* DebugHookManager()
{
    void* game{};
    void* manager{};
    if(!g_clientGameSlot || !ReadValue(g_clientGameSlot, 0, game) || !game
        || !g_debugHookManagerOffset
        || !ReadValue(game, g_debugHookManagerOffset, manager))
    {
        return nullptr;
    }
    return manager;
}

int PushDirectResult(void* lua, bool result)
{
    g_luaPushBoolean(lua, result ? 1 : 0);
    return 1;
}

int __cdecl DirectTriggerServerEvent(void* lua)
{
    if(!g_triggerServerEvent)
        return PushDirectResult(lua, false);
    DispatchEventCatchers(lua);
    SetNetcLuaCallContext(CurrentLuaResource(lua));
    const int result = g_triggerServerEvent(lua);
    ClearNetcLuaCallContext();
    return result;
}

int __cdecl DirectAddEvent(void* lua)
{
    return g_addEvent ? g_addEvent(lua) : PushDirectResult(lua, false);
}

int __cdecl DirectTriggerEvent(void* lua)
{
    return g_triggerEvent ? g_triggerEvent(lua) : PushDirectResult(lua, false);
}

int __cdecl DirectAddEventHandler(void* lua)
{
    return g_addEventHandler ? g_addEventHandler(lua)
        : PushDirectResult(lua, false);
}

int __cdecl DirectRemoveEventHandler(void* lua)
{
    return g_removeEventHandler ? g_removeEventHandler(lua)
        : PushDirectResult(lua, false);
}

bool CatcherId(void* lua, int index, std::uint32_t& id)
{
    const std::string text = LuaText(lua, index, 32, false);
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, id);
    return parsed.ec == std::errc{} && parsed.ptr == end && id;
}

int __cdecl DirectCatchServerEvent(void* lua)
{
    if(g_luaGetTop(lua) < 3 || g_luaType(lua, 1) != 4
        || g_luaType(lua, 3) != 6)
    {
        return PushDirectResult(lua, false);
    }

    const std::string name = LuaText(lua, 1, 512, false);
    int skip{};
    const std::string skipText = LuaText(lua, 2, 16, false);
    const auto parsed = std::from_chars(skipText.data(),
        skipText.data() + skipText.size(), skip);
    void* owner{};
    void* main{};
    if(name.empty() || name == "<value>" || parsed.ec != std::errc{}
        || parsed.ptr != skipText.data() + skipText.size() || skip < 0
        || skip > 255 || !LuaIdentity(lua, owner, main))
    {
        return PushDirectResult(lua, false);
    }

    std::uint32_t id = g_nextCatcher.fetch_add(1, std::memory_order_relaxed);
    if(!id)
        id = g_nextCatcher.fetch_add(1, std::memory_order_relaxed);
    const std::string key = "__darkFlameEventCatcher_" + std::to_string(id);
    g_luaPushValue(lua, 3);
    g_luaSetField(lua, LuaGlobalsIndex, key.c_str());
    {
        std::scoped_lock lock(g_catcherMutex);
        g_eventCatchers.push_back({id, owner, main, name, key, skip});
    }
    const std::string handle = std::to_string(id);
    g_luaPushString(lua, handle.c_str());
    return 1;
}

int __cdecl DirectRemoveEventCatcher(void* lua)
{
    std::uint32_t id{};
    void* owner{};
    void* main{};
    if(!CatcherId(lua, 1, id) || !LuaIdentity(lua, owner, main))
        return PushDirectResult(lua, false);

    std::string key;
    {
        std::scoped_lock lock(g_catcherMutex);
        const auto catcher = std::find_if(g_eventCatchers.begin(),
            g_eventCatchers.end(), [&](const EventCatcher& item)
        {
            return item.id == id && item.owner == owner && item.main == main;
        });
        if(catcher == g_eventCatchers.end())
            return PushDirectResult(lua, false);
        key = catcher->key;
        g_eventCatchers.erase(catcher);
    }
    g_luaPushNil(lua);
    g_luaSetField(lua, LuaGlobalsIndex, key.c_str());
    return PushDirectResult(lua, true);
}

bool CatcherActive(std::uint32_t id)
{
    std::scoped_lock lock(g_catcherMutex);
    return std::any_of(g_eventCatchers.begin(), g_eventCatchers.end(),
        [id](const EventCatcher& item) { return item.id == id; });
}

void DispatchEventCatchers(void* lua)
{
    if(g_dispatchingCatchers || !lua || g_luaGetTop(lua) < 1
        || g_luaType(lua, 1) != 4)
    {
        return;
    }

    void* owner{};
    void* main{};
    if(!LuaIdentity(lua, owner, main))
        return;
    const std::string name = LuaText(lua, 1, 512, false);
    std::vector<EventCatcher> matches;
    {
        std::scoped_lock lock(g_catcherMutex);
        std::copy_if(g_eventCatchers.begin(), g_eventCatchers.end(),
            std::back_inserter(matches), [&](const EventCatcher& item)
        {
            return item.owner == owner && item.main == main && item.name == name;
        });
    }
    if(matches.empty())
        return;

    const int top = g_luaGetTop(lua);
    g_dispatchingCatchers = true;
    for(const EventCatcher& catcher : matches)
    {
        if(!CatcherActive(catcher.id))
            continue;
        g_luaGetField(lua, LuaGlobalsIndex, catcher.key.c_str());
        if(g_luaType(lua, -1) != 6)
        {
            g_luaSetTop(lua, top);
            continue;
        }
        const int first = std::min(top + 1, 2 + catcher.skip);
        for(int index = first; index <= top; ++index)
            g_luaPushValue(lua, index);
        const int result = g_luaPCall(lua, top - first + 1, 0, 0);
        if(result)
        {
            Log::Write(L"[lua-bridge] event catcher callback failed: "
                + WideAscii(LuaText(lua, -1, 1024, false)));
        }
        g_luaSetTop(lua, top);
    }
    g_dispatchingCatchers = false;
}

HWND ProcessWindow()
{
    DWORD process{};
    HWND window = GetForegroundWindow();
    if(window)
        GetWindowThreadProcessId(window, &process);
    if(process == GetCurrentProcessId())
        return window;

    struct Search
    {
        DWORD process;
        HWND window;
    } search{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND candidate, LPARAM parameter) -> BOOL
    {
        auto& search = *reinterpret_cast<Search*>(parameter);
        DWORD process{};
        GetWindowThreadProcessId(candidate, &process);
        if(process != search.process || GetWindow(candidate, GW_OWNER)
            || !IsWindowVisible(candidate) || !GetWindowTextLengthW(candidate))
        {
            return TRUE;
        }
        search.window = candidate;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

int VirtualKey(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value)
    {
        return static_cast<char>(std::toupper(value));
    });
    if(key.size() == 1)
    {
        const SHORT code = VkKeyScanA(key.front());
        return code == -1 ? 0 : LOBYTE(code);
    }
    if(key == "SPACE") return VK_SPACE;
    if(key == "ENTER") return VK_RETURN;
    if(key == "LSHIFT") return VK_LSHIFT;
    if(key == "LCTRL") return VK_LCONTROL;
    if(key == "LALT") return VK_LMENU;
    return 0;
}

int __cdecl DirectEmulateKey(void* lua)
{
    const std::string key = LuaText(lua, 1, 32);
    const int virtualKey = VirtualKey(key);
    HWND window = FindWindowA(nullptr, "MTA: Province");
    if(!window) window = FindWindowA(nullptr, "MTA: San Andreas");
    if(!window) window = ProcessWindow();
    if(!virtualKey || !window)
        return PushDirectResult(lua, false);

    const bool pressed = g_luaToBoolean(lua, 2) != 0;
    const UINT scan = MapVirtualKeyA(virtualKey, MAPVK_VK_TO_VSC);
    LPARAM parameter = 1 | static_cast<LPARAM>(scan << 16);
    const bool extended = virtualKey == VK_RMENU || virtualKey == VK_RCONTROL
        || virtualKey == VK_NUMLOCK || virtualKey == VK_INSERT
        || virtualKey == VK_DELETE || virtualKey == VK_HOME
        || virtualKey == VK_END || virtualKey == VK_PRIOR
        || virtualKey == VK_NEXT || virtualKey == VK_UP
        || virtualKey == VK_DOWN || virtualKey == VK_LEFT
        || virtualKey == VK_RIGHT || virtualKey == VK_DIVIDE;
    if(extended)
        parameter |= static_cast<LPARAM>(1u << 24);
    if(!pressed)
        parameter |= static_cast<LPARAM>((1u << 30) | (1u << 31));
    const UINT message = virtualKey == VK_LMENU
        ? (pressed ? WM_SYSKEYDOWN : WM_SYSKEYUP)
        : (pressed ? WM_KEYDOWN : WM_KEYUP);
    return PushDirectResult(lua, PostMessageA(window, message, virtualKey,
        parameter) != FALSE);
}

int __cdecl DirectPlayAlertSignal(void* lua)
{
    return PushDirectResult(lua, PlayTramAlertSignal());
}

int DirectDebugHook(void* lua, bool add)
{
    int hookType{};
    if(!g_luaMToRef || !g_luaFunctionRefDtor || !g_luaType
        || g_luaGetTop(lua) < 2 || !DebugHookType(lua, hookType)
        || g_luaType(lua, 2) != 6)
    {
        return PushDirectResult(lua, false);
    }

    std::vector<std::string> allowedNames;
    if(add && !AllowedDebugHookNames(lua, allowedNames))
        return PushDirectResult(lua, false);

    void* manager = DebugHookManager();
    if(!manager)
        return PushDirectResult(lua, false);

    LuaFunctionRefStorage functionRef{};
    g_luaMToRef(&functionRef, lua, 2);
    const bool result = add
        ? g_addDebugHook && g_addDebugHook(manager, hookType, &functionRef,
            &allowedNames)
        : g_removeDebugHook && g_removeDebugHook(manager, hookType, &functionRef);
    g_luaFunctionRefDtor(&functionRef);
    return PushDirectResult(lua, result);
}

int __cdecl DirectAddDebugHook(void* lua)
{
    return DirectDebugHook(lua, true);
}

int __cdecl DirectRemoveDebugHook(void* lua)
{
    return DirectDebugHook(lua, false);
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
        "E.triggerServerEvent=B.dfTriggerServerEvent E.triggerEvent=B.dfTriggerEvent "
        "E.addEvent=B.dfAddEvent E.removeEventHandler=B.dfRemoveEventHandler "
        "E.addDebugHook=function(t,f,n) local o=B.dfAddDebugHook(t,f,n) "
        "if o then T(function() B.dfRemoveDebugHook(t,f) end) end return o end "
        "E.removeDebugHook=B.dfRemoveDebugHook "
        "E.dfCatchServerEvent=function(n,s,f) local o=B.dfCatchServerEvent(n,s,f) "
        "if o then T(function() B.dfRemoveEventCatcher(o) end) end return o end "
        "E.dfRemoveEventCatcher=B.dfRemoveEventCatcher "
        "E.setTimer=function(f,d,n,...) local t=B.setTimer(f,d,n,...) "
        "if t then T(function() if B.isTimer(t) then B.killTimer(t) end end) end return t end "
        "E.addEventHandler=function(n,e,f,...) local o=B.dfAddEventHandler(n,e,f,...) "
        "if o then T(function() B.dfRemoveEventHandler(n,e,f) end) end return o end "
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

    RegisterDirectAliases(target.state);

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
    g_tramBootstrapState.store(0, std::memory_order_release);
    g_tramRetryTick.store(0, std::memory_order_release);
    g_tramResourceSeen.store(false, std::memory_order_release);
    g_tramResourceThread.store(0, std::memory_order_release);
    g_tramFallbackLogged.store(false, std::memory_order_release);
    g_tramUiReadyLogged.store(false, std::memory_order_release);
    g_tramOwner = nullptr;
    g_tramMain = nullptr;
    g_sessions.clear();
    {
        std::scoped_lock lock(g_catcherMutex);
        g_eventCatchers.clear();
    }
    GuiClearLuaThreads();
    GuiResetTramState();
    g_hideCalls.store(false, std::memory_order_release);
    g_scopeDepth.store(0, std::memory_order_release);
}

bool ReadTramScript(std::string& code, std::string& error)
{
    code.clear();
    if(g_tramScriptPath.empty())
    {
        error = "DarkFlame directory is unavailable";
        return false;
    }
    Log::Write(L"[trambot] reading external script: " + g_tramScriptPath);
    HANDLE file = CreateFileW(g_tramScriptPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
    {
        error = "TramBot.lua not found beside DarkFlame.exe";
        return false;
    }
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(file);
        error = "TramBot.lua has an invalid size";
        return false;
    }
    code.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    const bool loaded = ReadFile(file, code.data(), static_cast<DWORD>(code.size()),
        &read, nullptr) && read == static_cast<DWORD>(code.size());
    CloseHandle(file);
    if(!loaded)
    {
        code.clear();
        error = "TramBot.lua read failed";
        return false;
    }
    if(code.size() >= 3 && static_cast<unsigned char>(code[0]) == 0xEF
        && static_cast<unsigned char>(code[1]) == 0xBB
        && static_cast<unsigned char>(code[2]) == 0xBF)
    {
        code.erase(0, 3);
    }
    if(code.find('\0') != std::string::npos)
    {
        code.clear();
        error = "TramBot.lua contains a zero byte";
        return false;
    }
    Log::Write(L"[trambot] external script read: "
        + std::to_wstring(code.size()) + L" bytes");
    return true;
}

void ForgetTramCatchers()
{
    if(!g_tramOwner || !g_tramMain)
        return;
    std::scoped_lock lock(g_catcherMutex);
    const auto end = std::remove_if(g_eventCatchers.begin(), g_eventCatchers.end(),
        [](const EventCatcher& item)
    {
        return item.owner == g_tramOwner && item.main == g_tramMain;
    });
    g_eventCatchers.erase(end, g_eventCatchers.end());
}

void TryStartTramBot(void* lua)
{
    if(!lua || !g_tramResourceSeen.load(std::memory_order_acquire)
        || g_tramResourceThread.load(std::memory_order_acquire)
            != GetCurrentThreadId()
        || CurrentLuaResource(lua) != "province_tram")
    {
        return;
    }
    if(g_tramBootstrapState.load(std::memory_order_acquire) == 2)
    {
        if(LuaStateAlive(g_tramMain, g_tramOwner))
        {
            g_tramResourceSeen.store(false, std::memory_order_release);
            return;
        }
        ForgetTramCatchers();
        g_tramOwner = nullptr;
        g_tramMain = nullptr;
        g_tramBootstrapState.store(0, std::memory_order_release);
        GuiResetTramState();
    }

    const DWORD now = GetTickCount();
    DWORD checked = g_tramRetryTick.load(std::memory_order_relaxed);
    if(now - checked < 1000 || !g_tramRetryTick.compare_exchange_strong(
        checked, now, std::memory_order_relaxed))
    {
        return;
    }
    std::uint32_t expected{};
    if(!g_tramBootstrapState.compare_exchange_strong(expected, 1,
        std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return;
    }

    std::string code;
    std::string error;
    void* owner{};
    void* main{};
    if(!LuaIdentity(lua, owner, main))
    {
        error = "province_tram VM identity is unavailable";
    }
    else if(ReadTramScript(code, error))
    {
        RegisterDirectAliases(lua);
        g_scopeDepth.fetch_add(1, std::memory_order_acq_rel);
        const bool started = RunLuaChunk(lua, code, error);
        g_scopeDepth.fetch_sub(1, std::memory_order_acq_rel);
        if(started)
        {
            g_tramOwner = owner;
            g_tramMain = main;
            g_tramResourceSeen.store(false, std::memory_order_release);
            g_tramBootstrapState.store(2, std::memory_order_release);
            Log::Write(L"[trambot] TramBot.lua started in province_tram: "
                + g_tramScriptPath);
            return;
        }
    }

    g_tramBootstrapState.store(0, std::memory_order_release);
    GuiUpdateTramState("loaded", "0");
    GuiUpdateTramState("status", error);
    static std::string previousError;
    if(error != previousError)
    {
        previousError = error;
        Log::Write(L"[trambot] startup pending: " + WideAscii(error));
    }
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
    DispatchEventCatchers(lua);
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
    const bool ready = g_ready.load(std::memory_order_acquire);
    const bool tramVm = ready && CurrentLuaResource(lua) == "province_tram";
    const bool relayArmed = tramVm
        && g_tramResourceSeen.load(std::memory_order_acquire)
        && g_tramResourceThread.load(std::memory_order_acquire)
            == GetCurrentThreadId();
    if(ready)
        ResetForReconnect();
    const bool tramCandidate = tramVm && (relayArmed
        || g_tramBootstrapState.load(std::memory_order_acquire) != 2);
    if(tramCandidate)
    {
        g_tramResourceSeen.store(true, std::memory_order_release);
        g_tramResourceThread.store(GetCurrentThreadId(), std::memory_order_release);
        g_tramRetryTick.store(0, std::memory_order_release);
        if(!relayArmed && !g_tramFallbackLogged.exchange(true))
            Log::Write(L"[trambot] province_tram VM reached via lua_pcall fallback");
    }
    if(ready)
        RunDirectBootstrap(lua);
    const int result = g_luaPCall(lua, argumentCount, resultCount, errorFunction);
    if(tramCandidate)
    {
        if(!result)
        {
            TryStartTramBot(lua);
        }
        else
        {
            g_tramResourceSeen.store(false, std::memory_order_release);
            Log::Write(L"[trambot] province_tram pcall failed; startup skipped ("
                + std::to_wstring(result) + L")");
        }
    }
    return result;
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
    g_triggerEvent = nullptr;
    g_addEvent = nullptr;
    g_addEventHandler = nullptr;
    g_removeEventHandler = nullptr;
    g_getVirtualMachine = nullptr;
    g_luaManagerSlot = nullptr;
    g_clientGameSlot = nullptr;
    g_debugHookManagerOffset = 0;
    g_luaType = nullptr;
    g_luaPushNil = nullptr;
    g_luaNext = nullptr;
    g_luaPushValue = nullptr;
    g_addDebugHook = nullptr;
    g_removeDebugHook = nullptr;
    g_luaMToRef = nullptr;
    g_luaFunctionRefDtor = nullptr;
    ClearNetcLuaCallContext();
    g_hideCalls.store(false, std::memory_order_release);
    g_scopeDepth.store(0, std::memory_order_release);
    g_monitorReady.store(false, std::memory_order_release);
    g_bootstrapState.store(0, std::memory_order_release);
    g_bootstrapLua.store(nullptr, std::memory_order_release);
    g_bootstrapMain.store(nullptr, std::memory_order_release);
    g_reconnectCheckTick.store(0, std::memory_order_release);
    g_tramBootstrapState.store(0, std::memory_order_release);
    g_tramRetryTick.store(0, std::memory_order_release);
    g_tramResourceSeen.store(false, std::memory_order_release);
    g_tramResourceThread.store(0, std::memory_order_release);
    g_tramFallbackLogged.store(false, std::memory_order_release);
    g_tramUiReadyLogged.store(false, std::memory_order_release);
    g_tramOwner = nullptr;
    g_tramMain = nullptr;
    g_sessions.clear();
    {
        std::scoped_lock lock(g_catcherMutex);
        g_eventCatchers.clear();
    }
    GuiClearLuaThreads();
    GuiResetTramState();
}

std::uintptr_t Find(const SignatureScanner& scanner, std::string_view pattern,
    const wchar_t* name)
{
    const std::uintptr_t address = scanner.Find(pattern);
    Log::Scan(name, address ? L"found" : L"not_found", address);
    return address;
}
}

bool PlayTramAlertSignal()
{
    static const std::vector<std::uint8_t> sound = []
    {
        constexpr std::uint32_t rate = 22050;
        constexpr std::uint32_t samples = rate * 6 / 5;
        constexpr double pi = 3.14159265358979323846;
        std::vector<std::uint8_t> wave;
        wave.reserve(44 + samples * 2);
        const auto bytes = [&](std::uint32_t value, unsigned count)
        {
            for(unsigned index = 0; index < count; ++index)
                wave.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
        };
        wave.insert(wave.end(), {'R', 'I', 'F', 'F'});
        bytes(36 + samples * 2, 4);
        wave.insert(wave.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        bytes(16, 4);
        bytes(1, 2);
        bytes(1, 2);
        bytes(rate, 4);
        bytes(rate * 2, 4);
        bytes(2, 2);
        bytes(16, 2);
        wave.insert(wave.end(), {'d', 'a', 't', 'a'});
        bytes(samples * 2, 4);
        for(std::uint32_t index = 0; index < samples; ++index)
        {
            const double time = static_cast<double>(index) / rate;
            const double phase = std::fmod(time, 0.15);
            const unsigned pulse = static_cast<unsigned>(time / 0.15);
            const double frequency = pulse % 2 ? 660.0 : 990.0;
            const double edge = std::min({phase * 30.0,
                (0.15 - phase) * 30.0, 1.0});
            const auto sample = static_cast<std::int16_t>(std::sin(
                2.0 * pi * frequency * time) * 15000.0 * edge);
            bytes(static_cast<std::uint16_t>(sample), 2);
        }
        return wave;
    }();
    HWND window = ProcessWindow();
    if(window)
    {
        FLASHWINFO flash{sizeof(flash), window, FLASHW_TRAY, 3, 0};
        FlashWindowEx(&flash);
    }
    return PlaySoundA(reinterpret_cast<LPCSTR>(sound.data()), nullptr,
        SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

void NotifyTramResourceScriptSeen(std::string_view path)
{
    g_tramResourceThread.store(GetCurrentThreadId(), std::memory_order_release);
    g_tramRetryTick.store(0, std::memory_order_release);
    if(!g_tramResourceSeen.exchange(true, std::memory_order_acq_rel))
    {
        Log::Write(L"[trambot] province_tram relay armed: "
            + WideAscii(std::string(path)));
    }
}

bool InstallLuaBridge(HMODULE client, std::wstring_view loaderDirectory)
{
    if(g_ready.load(std::memory_order_acquire))
        return true;
    std::scoped_lock lock(g_installMutex);
    if(g_ready.load(std::memory_order_relaxed))
        return true;

    g_tramScriptPath = loaderDirectory.empty() ? std::wstring{}
        : std::wstring(loaderDirectory) + L"\\TramBot.lua";
    Log::Write(g_tramScriptPath.empty()
        ? L"[trambot] external script path unavailable"
        : L"[trambot] external script configured: " + g_tramScriptPath);

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
    const std::uintptr_t addDebugHook = Find(scanner, AddDebugHookPattern,
        L"CDebugHookManager::AddDebugHook");
    const std::uintptr_t removeDebugHook = Find(scanner, RemoveDebugHookPattern,
        L"CDebugHookManager::RemoveDebugHook");
    const std::uintptr_t luaMToRef = Find(scanner, LuaMToRefPattern,
        L"luaM_toref");
    const std::uintptr_t luaFunctionRefDtor = Find(scanner,
        LuaFunctionRefDtorPattern, L"CLuaFunctionRef::~CLuaFunctionRef");
    const std::uintptr_t luaType = Find(scanner, LuaTypePattern, L"lua_type");
    const std::uintptr_t luaPushNil = Find(scanner, LuaPushNilPattern,
        L"lua_pushnil");
    const std::uintptr_t luaNext = Find(scanner, LuaNextPattern, L"lua_next");
    const std::uintptr_t luaPushValue = Find(scanner, LuaPushValuePattern,
        L"lua_pushvalue");
    const std::uintptr_t clientGameAccess = Find(scanner,
        ClientGameDebugHookAccessPattern, L"g_pClientGame debug-hook access");
    if(!isNameAllowed || !callHook || !luaPCall || !pushCClosure || !setField
        || !pushBoolean || !toBoolean || !toLString || !pushString || !getTop
        || !getField || !setTop || !newThread || !luaLRef || !luaLUnref
        || !getLuaFunction || !getVirtualMachine || !luaManagerLoad
        || !addDebugHook || !removeDebugHook || !luaMToRef
        || !luaFunctionRefDtor || !luaType || !luaPushNil || !luaNext
        || !luaPushValue
        || !clientGameAccess)
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
    g_luaType = reinterpret_cast<LuaTypeFn>(luaType);
    g_luaPushNil = reinterpret_cast<LuaPushNilFn>(luaPushNil);
    g_luaNext = reinterpret_cast<LuaNextFn>(luaNext);
    g_luaPushValue = reinterpret_cast<LuaPushValueFn>(luaPushValue);
    g_addDebugHook = reinterpret_cast<AddDebugHookFn>(addDebugHook);
    g_removeDebugHook = reinterpret_cast<RemoveDebugHookFn>(removeDebugHook);
    g_luaMToRef = reinterpret_cast<LuaMToRefFn>(luaMToRef);
    g_luaFunctionRefDtor = reinterpret_cast<LuaFunctionRefDtorFn>(
        luaFunctionRefDtor);
    g_getVirtualMachine = reinterpret_cast<GetVirtualMachineFn>(getVirtualMachine);
    g_luaManagerSlot = *reinterpret_cast<void***>(luaManagerLoad + 2);
    g_clientGameSlot = *reinterpret_cast<void***>(clientGameAccess + 1);
    g_debugHookManagerOffset = *reinterpret_cast<const std::uint32_t*>(
        clientGameAccess + 27);
    if(!g_clientGameSlot || !g_debugHookManagerOffset
        || g_debugHookManagerOffset > 0x10000)
    {
        Log::Write(L"[lua-bridge] debug-hook manager access decode failed");
        return false;
    }
    g_isNameAllowedTarget = reinterpret_cast<void*>(isNameAllowed);
    g_callHookTarget = reinterpret_cast<void*>(callHook);
    g_luaPCallTarget = reinterpret_cast<void*>(luaPCall);
    for(DWORD waited = 0; waited <= 5000
        && (!g_triggerServerEventTarget || !g_triggerEvent || !g_addEvent
            || !g_addEventHandler || !g_removeEventHandler);
        waited += 10)
    {
        if(!g_triggerServerEventTarget)
        {
            g_triggerServerEventTarget = reinterpret_cast<void*>(
                FindRegisteredLuaFunction(getLuaFunction, "triggerServerEvent"));
        }
        if(!g_addEvent)
            g_addEvent = FindRegisteredLuaFunction(getLuaFunction, "addEvent");
        if(!g_addEventHandler)
        {
            g_addEventHandler = FindRegisteredLuaFunction(getLuaFunction,
                "addEventHandler");
        }
        if(!g_triggerEvent)
            g_triggerEvent = FindRegisteredLuaFunction(getLuaFunction,
                "triggerEvent");
        if(!g_removeEventHandler)
        {
            g_removeEventHandler = FindRegisteredLuaFunction(getLuaFunction,
                "removeEventHandler");
        }
        if((!g_triggerServerEventTarget || !g_triggerEvent || !g_addEvent
            || !g_addEventHandler || !g_removeEventHandler) && waited != 5000)
        {
            Sleep(10);
        }
    }
    Log::Scan(L"CLuaFunctionDefs::TriggerServerEvent",
        g_triggerServerEventTarget ? L"resolved" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_triggerServerEventTarget));
    Log::Scan(L"CLuaFunctionDefs::AddEvent",
        g_addEvent ? L"resolved" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_addEvent));
    Log::Scan(L"CLuaFunctionDefs::AddEventHandler",
        g_addEventHandler ? L"resolved" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_addEventHandler));
    Log::Scan(L"CLuaFunctionDefs::TriggerEvent",
        g_triggerEvent ? L"resolved" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_triggerEvent));
    Log::Scan(L"CLuaFunctionDefs::RemoveEventHandler",
        g_removeEventHandler ? L"resolved" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_removeEventHandler));
    if(!g_triggerServerEventTarget || !g_triggerEvent || !g_addEvent
        || !g_addEventHandler || !g_removeEventHandler)
    {
        Log::Write(L"[lua-bridge] direct alias registry lookup failed");
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
    g_tramBootstrapState.store(0, std::memory_order_release);
    g_tramRetryTick.store(0, std::memory_order_release);
    g_tramResourceSeen.store(false, std::memory_order_release);
    g_tramResourceThread.store(0, std::memory_order_release);
    g_tramFallbackLogged.store(false, std::memory_order_release);
    g_tramUiReadyLogged.store(false, std::memory_order_release);
    g_tramOwner = nullptr;
    g_tramMain = nullptr;
    GuiResetTramState();
    g_ready.store(true, std::memory_order_release);
    Log::Write(L"[lua-bridge] targeted packet context, native monitor, and Lua bridge ready");
    return true;
}
