#pragma once

#include <Windows.h>

bool InstallTextDisplayHook(HMODULE client);
void ResetTextDisplayHook(HMODULE client);
