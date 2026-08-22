#include "embedded_lua_loader.h"
#include "embedded_lua_runtime.h"

#include "third_party/mta_lua/lua.h"

#include <stdlib.h>

static void* SelfTestAllocator(void* data, void* block, size_t oldSize,
    size_t newSize)
{
    (void)data;
    (void)oldSize;
    if(!newSize)
    {
        free(block);
        return NULL;
    }
    return realloc(block, newSize);
}

static int SelfTestFunction(lua_State* lua)
{
    lua_pushinteger(lua, 42);
    return 1;
}

int DarkFlameLuaParserSelfTest(void)
{
    static const char source[] = "return parentValue + dfSelfTest()";
    lua_State* lua = lua_newstate(SelfTestAllocator, NULL, NULL);
    lua_State* thread;
    int reference;
    int status;
    int passed;
    if(!lua)
        return 0;
    lua_pushinteger(lua, 7);
    lua_setfield(lua, LUA_GLOBALSINDEX, "parentValue");
    thread = lua_newthread(lua);
    reference = DarkFlameLuaRef(lua, LUA_REGISTRYINDEX);
    DarkFlameLuaInstallPrivateGlobals(thread);
    DarkFlameLuaRegister(thread, "dfSelfTest",
        (DarkFlameLuaCFunction)SelfTestFunction);
    lua_getfield(lua, LUA_GLOBALSINDEX, "dfSelfTest");
    passed = lua_isnil(lua, -1);
    lua_pop(lua, 1);
    status = DarkFlameLoadLuaSource(thread, source, sizeof(source) - 1,
        "@DarkFlameSelfTest");
    if(!status)
        status = DarkFlameLuaPCall(thread, 0, 1, 0);
    passed = passed && !status && lua_tointeger(thread, -1) == 49;
    DarkFlameLuaUnref(lua, LUA_REGISTRYINDEX, reference);
    lua_close(lua);
    return passed;
}
