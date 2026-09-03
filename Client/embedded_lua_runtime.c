#include "embedded_lua_runtime.h"

#include "third_party/mta_lua/lua.h"

#define DFL_FREELIST_REF 0
#define DFL_REF_NIL (-1)

int DarkFlameLuaPCall(void* state, int arguments, int results,
    int errorFunction)
{
    return lua_pcall((lua_State*)state, arguments, results, errorFunction);
}

int DarkFlameLuaGetTop(void* state)
{
    return lua_gettop((lua_State*)state);
}

void DarkFlameLuaSetTop(void* state, int index)
{
    lua_settop((lua_State*)state, index);
}

int DarkFlameLuaIsNumber(void* state, int index)
{
    return lua_isnumber((lua_State*)state, index);
}

double DarkFlameLuaToNumber(void* state, int index)
{
    return lua_tonumber((lua_State*)state, index);
}

const char* DarkFlameLuaToLString(void* state, int index, size_t* size)
{
    return lua_tolstring((lua_State*)state, index, size);
}

void DarkFlameLuaSetField(void* state, int index, const char* name)
{
    lua_setfield((lua_State*)state, index, name);
}

void DarkFlameLuaGetField(void* state, int index, const char* name)
{
    lua_getfield((lua_State*)state, index, name);
}

void DarkFlameLuaPushBoolean(void* state, int value)
{
    lua_pushboolean((lua_State*)state, value);
}

int DarkFlameLuaToBoolean(void* state, int index)
{
    return lua_toboolean((lua_State*)state, index);
}

void DarkFlameLuaPushString(void* state, const char* value)
{
    lua_pushstring((lua_State*)state, value);
}

int DarkFlameLuaType(void* state, int index)
{
    return lua_type((lua_State*)state, index);
}

void DarkFlameLuaPushNil(void* state)
{
    lua_pushnil((lua_State*)state);
}

int DarkFlameLuaNext(void* state, int index)
{
    return lua_next((lua_State*)state, index);
}

void DarkFlameLuaPushValue(void* state, int index)
{
    lua_pushvalue((lua_State*)state, index);
}

void DarkFlameLuaPushCFunction(void* state, DarkFlameLuaCFunction function)
{
    lua_pushcclosure((lua_State*)state, (lua_CFunction)function, 0);
}

void DarkFlameLuaInsert(void* state, int index)
{
    lua_insert((lua_State*)state, index);
}

void DarkFlameLuaRemove(void* state, int index)
{
    lua_remove((lua_State*)state, index);
}

static int AbsoluteIndex(lua_State* lua, int index)
{
    return index > 0 || index <= LUA_REGISTRYINDEX
        ? index : lua_gettop(lua) + index + 1;
}

int DarkFlameLuaRef(void* state, int tableIndex)
{
    lua_State* lua = (lua_State*)state;
    int reference;
    tableIndex = AbsoluteIndex(lua, tableIndex);
    if(lua_isnil(lua, -1))
    {
        lua_pop(lua, 1);
        return DFL_REF_NIL;
    }
    lua_rawgeti(lua, tableIndex, DFL_FREELIST_REF);
    reference = (int)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    if(reference)
    {
        lua_rawgeti(lua, tableIndex, reference);
        lua_rawseti(lua, tableIndex, DFL_FREELIST_REF);
    }
    else
        reference = (int)lua_objlen(lua, tableIndex) + 1;
    lua_rawseti(lua, tableIndex, reference);
    return reference;
}

void DarkFlameLuaUnref(void* state, int tableIndex, int reference)
{
    lua_State* lua = (lua_State*)state;
    if(reference < 0)
        return;
    tableIndex = AbsoluteIndex(lua, tableIndex);
    lua_rawgeti(lua, tableIndex, DFL_FREELIST_REF);
    lua_rawseti(lua, tableIndex, reference);
    lua_pushinteger(lua, reference);
    lua_rawseti(lua, tableIndex, DFL_FREELIST_REF);
}

void DarkFlameLuaPushRef(void* state, int tableIndex, int reference)
{
    lua_rawgeti((lua_State*)state, tableIndex, reference);
}

int DarkFlameLuaInstallPrivateGlobals(void* state)
{
    lua_State* lua = (lua_State*)state;
    int environment;
    if(!lua)
        return 0;
    lua_newtable(lua);
    environment = lua_gettop(lua);
    lua_pushvalue(lua, environment);
    lua_setfield(lua, environment, "_G");
    lua_newtable(lua);
    lua_pushvalue(lua, LUA_GLOBALSINDEX);
    lua_setfield(lua, -2, "__index");
    lua_setmetatable(lua, environment);
    lua_replace(lua, LUA_GLOBALSINDEX);
    return 1;
}

void DarkFlameLuaRegister(void* state, const char* name,
    DarkFlameLuaCFunction function)
{
    lua_State* lua = (lua_State*)state;
    lua_pushcclosure(lua, (lua_CFunction)function, 0);
    lua_setfield(lua, LUA_GLOBALSINDEX, name);
}
