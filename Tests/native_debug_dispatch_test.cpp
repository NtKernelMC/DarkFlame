extern "C" {
#include "../Client/third_party/mta_lua/lua.h"
}
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

std::atomic_bool g_hideCalls{};
std::atomic_uint32_t g_scopeDepth{};
std::atomic_bool g_eventLogged{};
int dispatches{}, natives{}, posts{};
bool originalAllows{};
using CallHookFn = bool(__stdcall*)(const char*, const void*, const void*, bool);
bool __stdcall OriginalDispatch(const char*, const void*, const void*, bool) {
    ++dispatches;
    return originalAllows;
}
CallHookFn g_callHook = OriginalDispatch;
namespace Log { void Write(const wchar_t*) {} }
void GuiAppendEvent(const std::string&) {}
bool NativeEventRow(const char*, const void*, std::string&) { return false; }

#include "native_debug_dispatch_production.inc"

int TickNative(lua_State* state) {
    ++natives;
    lua_pushnumber(state, 123456);
    lua_pushboolean(state, 0);
    lua_pushnil(state);
    lua_pushstring(state, "native result");
    return 4;
}
int PreCall(lua_CFunction function, lua_State*) {
    if(function != TickNative) std::abort();
    return HookCallHook("getTickCount", nullptr, nullptr, false);
}
void PostCall(lua_CFunction, lua_State*) { ++posts; }
void* Allocate(void*, void* block, size_t, size_t size) {
    if(!size) { std::free(block); return nullptr; }
    return std::realloc(block, size);
}
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"Failed line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    lua_State* state = lua_newstate(Allocate, nullptr, nullptr);
    CHECK(state);
    lua_registerPreCallHook(PreCall);
    lua_registerPostCallHook(PostCall);
    for(int mode=0; mode<5; ++mode) {
        g_hideCalls = mode == 2;
        g_scopeDepth = mode == 3 ? 2 : 0;
        originalAllows = mode == 1;
        dispatches = natives = posts = 0;
        lua_settop(state, 0);
        lua_pushcfunction(state, TickNative);
        CHECK(lua_pcall(state, 0, 4, 0) == 0);
        CHECK(lua_gettop(state) == 4);
        const bool executes = mode == 1 || mode == 2 || mode == 3;
        CHECK(dispatches == (mode == 2 || mode == 3 ? 0 : 1));
        CHECK(natives == (executes ? 1 : 0));
        CHECK(posts == (executes ? 1 : 0));
        if(executes) {
            CHECK(lua_type(state, 1) == LUA_TNUMBER && lua_tonumber(state, 1) == 123456);
            CHECK(lua_type(state, 2) == LUA_TBOOLEAN && !lua_toboolean(state, 2));
            CHECK(lua_type(state, 3) == LUA_TNIL);
            CHECK(std::string(lua_tolstring(state, 4, nullptr)) == "native result");
        } else {
            for(int i=1;i<=4;++i) CHECK(lua_type(state, i) == LUA_TNIL);
        }
    }
    lua_registerPreCallHook(nullptr);
    lua_registerPostCallHook(nullptr);
    lua_close(state);
    std::puts("PASS: production dispatch detour + MTA Lua: visible decisions, hidden callbacks, native execution, four return values, scope restoration");
    return 0;
}
