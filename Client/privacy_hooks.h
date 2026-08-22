#pragma once

#include <Windows.h>

namespace PrivacyFlags
{
namespace Registry
{
inline constexpr bool Enabled = true;
inline constexpr bool HardwareProfileGuid = true;
inline constexpr bool ClientHwid = true;
inline constexpr bool SqmMachineId = true;
inline constexpr bool WindowsProductId = true;
inline constexpr bool MountedSystemDrive = true;
inline constexpr bool MachineGuid = true;
inline constexpr bool ProcessorName = true;
inline constexpr bool ProcessorVendor = true;
inline constexpr bool ProcessorIdentifier = true;
inline constexpr bool ProcessorRevision = true;
inline constexpr bool BaseBoardManufacturer = true;
inline constexpr bool BaseBoardProduct = true;
inline constexpr bool BaseBoardSerial = true;
inline constexpr bool BiosSerial = true;
inline constexpr bool BiosReleaseDate = true;
inline constexpr bool BiosVendor = true;
inline constexpr bool BiosVersion = true;
inline constexpr bool AutoLogonSid = true;
inline constexpr bool EdgeInstallTime = true;
inline constexpr bool PrimaryAdapter = true;
}

namespace Wmi
{
inline constexpr bool Enabled = true;
inline constexpr bool VideoCaption = true;
inline constexpr bool VideoName = true;
inline constexpr bool VideoDescription = true;
inline constexpr bool BaseBoardSerial = true;
inline constexpr bool BaseBoardProduct = true;
inline constexpr bool BaseBoardManufacturer = true;
inline constexpr bool BiosSerial = true;
inline constexpr bool BiosManufacturer = true;
inline constexpr bool BiosSmbiosVersion = true;
inline constexpr bool BiosVersion = true;
inline constexpr bool BiosReleaseDate = true;
inline constexpr bool ProcessorName = true;
inline constexpr bool ProcessorManufacturer = true;
inline constexpr bool ProcessorDescription = true;
inline constexpr bool ProcessorId = true;
inline constexpr bool SystemProductUuid = true;
inline constexpr bool SystemProductIdentifyingNumber = true;
inline constexpr bool SystemProductVendor = true;
inline constexpr bool SystemProductName = true;
inline constexpr bool ComputerSystemManufacturer = true;
inline constexpr bool ComputerSystemModel = true;
}

namespace Adapters
{
inline constexpr bool Enabled = true;
inline constexpr bool MacAddress = true;
}
}

bool StartPrivacyHooks(HMODULE client);
void UpdatePrivacyClient(HMODULE client);
bool RepairPrivacyHooks();
