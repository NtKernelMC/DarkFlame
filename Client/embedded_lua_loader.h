#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int DarkFlameLoadLuaSource(void* lua, const char* source, size_t size,
    const char* chunkName);
int DarkFlameLuaParserSelfTest(void);

#ifdef __cplusplus
}
#endif
