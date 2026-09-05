#pragma once

#include <Windows.h>

#include <string_view>

bool ConfigureNetcHooks(bool setSerial, bool randomSerial,
    std::string_view publicSerial);
bool InstallNetcHooks(HMODULE netc);
void ResetNetcHooks(HMODULE netc);
bool InitializeNetcClientApi(HMODULE client);
void ResetNetcClientApi(HMODULE client);
void SetNetcLuaCallContext(std::string_view resource);
void ClearNetcLuaCallContext();
bool ReadLocalPlayerPosition(float& x, float& y, float& z);
bool SendReliablePlayerPureSync(float x, float y, float z);
bool SendVehicleSelfLink(unsigned int vehicleId);
void BeginPlayerPureSyncSuppression();
void EndPlayerPureSyncSuppression();
