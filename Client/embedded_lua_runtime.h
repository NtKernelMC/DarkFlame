#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*DarkFlameLuaCFunction)(void* lua);

int DarkFlameLuaGetTop(void* lua);
void DarkFlameLuaSetTop(void* lua, int index);
int DarkFlameLuaIsNumber(void* lua, int index);
double DarkFlameLuaToNumber(void* lua, int index);
const char* DarkFlameLuaToLString(void* lua, int index, size_t* size);
void DarkFlameLuaSetField(void* lua, int index, const char* name);
void DarkFlameLuaGetField(void* lua, int index, const char* name);
void DarkFlameLuaPushBoolean(void* lua, int value);
int DarkFlameLuaToBoolean(void* lua, int index);
void DarkFlameLuaPushString(void* lua, const char* value);
int DarkFlameLuaType(void* lua, int index);
void DarkFlameLuaPushNil(void* lua);
int DarkFlameLuaNext(void* lua, int index);
void DarkFlameLuaPushValue(void* lua, int index);
void DarkFlameLuaPushCFunction(void* lua, DarkFlameLuaCFunction function);
void DarkFlameLuaInsert(void* lua, int index);
void DarkFlameLuaRemove(void* lua, int index);
int DarkFlameLuaRef(void* lua, int tableIndex);
void DarkFlameLuaUnref(void* lua, int tableIndex, int reference);
void DarkFlameLuaPushRef(void* lua, int tableIndex, int reference);
int DarkFlameLuaInstallPrivateGlobals(void* lua);
void DarkFlameLuaRegister(void* lua, const char* name,
    DarkFlameLuaCFunction function);
int DarkFlameLuaPCall(void* lua, int arguments, int results,
    int errorFunction);

#ifdef __cplusplus
}
#endif
