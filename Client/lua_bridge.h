#pragma once

#include <Windows.h>

#include <string_view>

bool InstallLuaBridge(HMODULE client, std::wstring_view loaderDirectory);
bool RepairLuaBridgeHooks(HMODULE client);
void ResetLuaBridgeHooks(HMODULE client);
void PulseLuaBridge();
bool PlayTramAlertSignal();
