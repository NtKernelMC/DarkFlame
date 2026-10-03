#include <Windows.h>

extern "C"
{
#include "../Client/third_party/mta_lua/lua.h"
#include "../Client/third_party/mta_lua/lstate.h"
#include "../Client/embedded_lua_loader.h"
}

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

struct Allocations
{
    std::unordered_map<void*, size_t> live;
    int errors{};
    int failAfter{-1};
};

void* Allocate(void* context, void* block, size_t oldSize, size_t newSize)
{
    auto& state = *static_cast<Allocations*>(context);
    if(block && (!state.live.count(block) || state.live[block] != oldSize))
    {
        std::fprintf(stderr, "Allocator mismatch: block=%p requestedOld=%zu actualOld=%zu new=%zu\n",
            block, oldSize, state.live.count(block) ? state.live[block] : 0, newSize);
        ++state.errors;
        return nullptr;
    }
    if(newSize && state.failAfter >= 0 && state.failAfter-- == 0)
        return nullptr;
    if(!newSize)
    {
        state.live.erase(block);
        std::free(block);
        return nullptr;
    }
    void* result = std::realloc(block, newSize);
    if(result)
    {
        state.live.erase(block);
        state.live[result] = newSize;
    }
    return result;
}

int NewTable(lua_State* lua)
{
    lua_createtable(lua, 8, 4);
    return 0;
}

#define HOST_API(name) const auto host_##name = reinterpret_cast<decltype(&lua_##name)>(GetProcAddress(host, "lua_" #name)); if(!host_##name) return 2
#define CHECK(condition) do { if(!(condition)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #condition); return 3; } } while(0)

int main()
{
    HMODULE host = LoadLibraryW(L"LuaTableHost.dll");
    if(!host) return 1;
    HOST_API(newstate);
    HOST_API(close);
    HOST_API(createtable);
    HOST_API(pushinteger);
    HOST_API(setfield);
    HOST_API(rawseti);
    HOST_API(pcall);
    HOST_API(gc);

    Allocations memory[2];
    lua_State* states[2]{};
    const Node* hostDummy{};
    for(int index = 0; index < 2; ++index)
    {
        lua_State* lua = states[index] = host_newstate(Allocate, &memory[index], nullptr);
        CHECK(lua);
        host_createtable(lua, 0, 0);
        const auto* table = hvalue(lua->top - 1);
        hostDummy = table->node;
        CHECK(table->lastfree == table->node);
        lua_pushinteger(lua, index);
        lua_setfield(lua, -2, "fromEmbedded");
        lua_setfield(lua, LUA_GLOBALSINDEX, "hostTable");

        const std::string source = "local saved={{" + std::to_string(index + 10)
            + "}} return function(x) local tmp={} tmp.name=x return saved[1][1]+tmp.name end";
        CHECK(!DarkFlameLoadLuaSource(lua, source.data(), source.size(), "@interop-test"));
        CHECK(!lua_pcall(lua, 0, 1, 0));
        lua_setfield(lua, LUA_GLOBALSINDEX, "callback");
    }

    for(int round = 0; round < 300; ++round)
    {
        for(int index = 0; index < 2; ++index)
        {
            auto* lua = states[index];
            lua_createtable(lua, 0, 0);
            CHECK(hvalue(lua->top - 1)->node != hostDummy);
            host_pushinteger(lua, round);
            host_rawseti(lua, -2, 1);
            host_pushinteger(lua, index);
            host_setfield(lua, -2, "state");
            lua_getfield(lua, -1, "state");
            CHECK(lua_tointeger(lua, -1) == index);
            lua_settop(lua, 0);

            host_createtable(lua, 0, 0);
            lua_pushinteger(lua, round);
            lua_setfield(lua, -2, "round");
            lua_createtable(lua, 0, 0);
            lua_settop(lua, 0);
            host_createtable(lua, 0, 0);
            lua_settop(lua, 0);
            if(round % 2) host_gc(lua, LUA_GCCOLLECT, 0);
            else lua_gc(lua, LUA_GCCOLLECT, 0);
            lua_getfield(lua, LUA_GLOBALSINDEX, "callback");
            host_pushinteger(lua, round);
            CHECK(!host_pcall(lua, 1, 1, 0));
            CHECK(lua_tointeger(lua, -1) == round + index + 10);
            lua_settop(lua, 0);
            CHECK(!memory[index].errors);
        }
    }
    const auto* sentinelBytes = reinterpret_cast<const volatile unsigned char*>(hostDummy);
    for(size_t i = 0; i < sizeof(Node); ++i) CHECK(sentinelBytes[i] == 0);

    for(int index = 0; index < 2; ++index)
    {
        auto* lua = states[index];
        for(int failure = 0; failure < 5; ++failure)
        {
            lua_gc(lua, LUA_GCSTOP, 0);
            memory[index].failAfter = failure;
            const int status = lua_cpcall(lua, NewTable, nullptr);
            memory[index].failAfter = -1;
            CHECK(status == LUA_ERRMEM);
            lua_settop(lua, 0);
            host_gc(lua, LUA_GCCOLLECT, 0);
            CHECK(!memory[index].errors);
        }
        host_close(lua);
        CHECK(!memory[index].errors && memory[index].live.empty());
    }
    FreeLibrary(host);
    std::puts("PASS: two Lua runtimes, isolated states, mixed GC, sentinel unchanged, allocation failures");
    return 0;
}
