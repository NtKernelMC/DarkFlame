#include "embedded_lua_loader.h"

#include "third_party/mta_lua/ldo.h"
#include "third_party/mta_lua/lfunc.h"
#include "third_party/mta_lua/lgc.h"
#include "third_party/mta_lua/lobject.h"
#include "third_party/mta_lua/lparser.h"
#include "third_party/mta_lua/lstate.h"
#include "third_party/mta_lua/lzio.h"

#include <stddef.h>

#define DFL_LAYOUT_ASSERT(name, expression) \
    typedef char name[(expression) ? 1 : -1]

DFL_LAYOUT_ASSERT(dfl_state_size, sizeof(lua_State) == 0x88);
DFL_LAYOUT_ASSERT(dfl_state_base, offsetof(lua_State, base) == 0x0C);
DFL_LAYOUT_ASSERT(dfl_state_global, offsetof(lua_State, l_G) == 0x10);
DFL_LAYOUT_ASSERT(dfl_state_ci, offsetof(lua_State, ci) == 0x14);
DFL_LAYOUT_ASSERT(dfl_state_stack, offsetof(lua_State, stack) == 0x18);
DFL_LAYOUT_ASSERT(dfl_state_savedpc, offsetof(lua_State, savedpc) == 0x20);
DFL_LAYOUT_ASSERT(dfl_state_base_ci, offsetof(lua_State, base_ci) == 0x24);
DFL_LAYOUT_ASSERT(dfl_state_nccalls, offsetof(lua_State, nCcalls) == 0x2C);
DFL_LAYOUT_ASSERT(dfl_state_stack_last, offsetof(lua_State, stack_last) == 0x34);
DFL_LAYOUT_ASSERT(dfl_state_size_ci, offsetof(lua_State, size_ci) == 0x3C);
DFL_LAYOUT_ASSERT(dfl_state_env, offsetof(lua_State, env) == 0x50);
DFL_LAYOUT_ASSERT(dfl_state_openupval, offsetof(lua_State, openupval) == 0x60);
DFL_LAYOUT_ASSERT(dfl_state_globals, offsetof(lua_State, l_gt) == 0x68);
DFL_LAYOUT_ASSERT(dfl_state_error_jmp, offsetof(lua_State, errorJmp) == 0x78);
DFL_LAYOUT_ASSERT(dfl_state_top, offsetof(lua_State, top) == 0x7C);
DFL_LAYOUT_ASSERT(dfl_state_errfunc, offsetof(lua_State, errfunc) == 0x80);

typedef struct DarkFlameSourceReader
{
    const char* source;
    size_t size;
    int delivered;
} DarkFlameSourceReader;

typedef struct DarkFlameSourceParser
{
    ZIO stream;
    Mbuffer buffer;
    const char* name;
} DarkFlameSourceParser;

static const char* ReadSource(lua_State* lua, void* data, size_t* size)
{
    DarkFlameSourceReader* reader = (DarkFlameSourceReader*)data;
    (void)lua;
    if(reader->delivered)
    {
        *size = 0;
        return NULL;
    }
    reader->delivered = 1;
    *size = reader->size;
    return reader->source;
}

static void ParseSource(lua_State* lua, void* data)
{
    DarkFlameSourceParser* parser = (DarkFlameSourceParser*)data;
    Proto* prototype;
    Closure* closure;
    int index;

    luaC_checkGC(lua);
    prototype = luaY_parser(lua, &parser->stream, &parser->buffer,
        parser->name);
    closure = luaF_newLclosure(lua, prototype->nups, hvalue(gt(lua)));
    closure->l.p = prototype;
    for(index = 0; index < prototype->nups; ++index)
        closure->l.upvals[index] = luaF_newupval(lua);
    setclvalue(lua, lua->top, closure);
    incr_top(lua);
}

int DarkFlameLoadLuaSource(void* state, const char* source, size_t size,
    const char* chunkName)
{
    lua_State* lua = (lua_State*)state;
    DarkFlameSourceReader reader;
    DarkFlameSourceParser parser;
    int status;

    if(!lua || (!source && size))
        return LUA_ERRSYNTAX;

    reader.source = source ? source : "";
    reader.size = size;
    reader.delivered = 0;
    parser.buffer.buffer = NULL;
    parser.buffer.n = 0;
    parser.buffer.buffsize = 0;
    parser.name = chunkName && *chunkName ? chunkName : "@DarkFlame";
    luaZ_init(lua, &parser.stream, ReadSource, &reader);
    status = luaD_pcall(lua, ParseSource, &parser,
        savestack(lua, lua->top), lua->errfunc);
    luaZ_freebuffer(lua, &parser.buffer);
    return status;
}
