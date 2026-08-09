#include "netc_hooks.h"

#include "hook_utils.h"
#include "netc_signatures.h"
#include "signature_scanner.h"

#include <array>

namespace
{
    using SendPacket = bool(__thiscall*)(void*, unsigned char, void*, int, int, int);

    SRWLOCK g_installLock = SRWLOCK_INIT;
    SendPacket g_sendPacket{};
    bool g_installed{};

    void __fastcall HookVpnBypass(DWORD*, void*, char) {}
    bool __cdecl HookIsNetworkConnected(DWORD*, DWORD*) { return false; }
    bool __fastcall HookIsNotViolationCode(void*, void*, unsigned int) { return true; }
    int __fastcall HookExecuteSecurityViolationKick(void*, void*) { return 0; }
    int __fastcall HookSendClientKick(void*, void*, char) { return 1; }
    int __cdecl HookSendLogger(int, void*, int, int, void*) { return 0; }
    int __fastcall HookSetClientKick(void*, void*, void*, void*, char, int) { return 0; }
    void __fastcall HookVfB00Z00Scanner(void*, void*, int) {}
    int __fastcall HookSetClientKickNew(void*, void*, char) { return 0; }
    int __fastcall HookScanModuleIntegrity(void*, void*, DWORD*) { return 0; }

    bool __fastcall HookSendPacket(void* self, void*, unsigned char packetId,
        void* bitStream, int priority, int reliability, int ordering)
    {
        const bool blocked = packetId == 34 || packetId == 91
            || packetId == 92 || packetId == 94;
        return blocked || g_sendPacket(self, packetId, bitStream,
            priority, reliability, ordering);
    }
}

bool InstallNetcHooks(HMODULE netc)
{
    AcquireSRWLockExclusive(&g_installLock);
    if (g_installed)
    {
        ReleaseSRWLockExclusive(&g_installLock);
        return true;
    }
    if (!netc)
    {
        ReleaseSRWLockExclusive(&g_installLock);
        return false;
    }

    const SignatureScanner scanner(netc);
    std::array hooks{
        HookUtils::Hook{L"AC__IsVpnEnabled", NetcSignatures::VpnBypass,
            reinterpret_cast<void*>(&HookVpnBypass)},
        HookUtils::Hook{L"AC__IsNetworkConnected", NetcSignatures::IsNetworkConnected,
            reinterpret_cast<void*>(&HookIsNetworkConnected)},
        HookUtils::Hook{L"AC__IsNotViolationCode", NetcSignatures::IsNotViolationCode,
            reinterpret_cast<void*>(&HookIsNotViolationCode)},
        HookUtils::Hook{L"AC_ExecuteSecurityViolationKick", NetcSignatures::ExecuteSecurityViolationKick,
            reinterpret_cast<void*>(&HookExecuteSecurityViolationKick)},
        HookUtils::Hook{L"AC__SendClientKick", NetcSignatures::SendClientKick,
            reinterpret_cast<void*>(&HookSendClientKick)},
        HookUtils::Hook{L"AC__SendLoggerToServerMtaDev", NetcSignatures::SendLogger,
            reinterpret_cast<void*>(&HookSendLogger)},
        HookUtils::Hook{L"AC_SetClientKick", NetcSignatures::SetClientKick,
            reinterpret_cast<void*>(&HookSetClientKick)},
        HookUtils::Hook{L"AC__VfB00_Z00Scanner", NetcSignatures::VfB00Z00Scanner,
            reinterpret_cast<void*>(&HookVfB00Z00Scanner)},
        HookUtils::Hook{L"AC__SetClientKickNew", NetcSignatures::SetClientKickNew,
            reinterpret_cast<void*>(&HookSetClientKickNew)},
        HookUtils::Hook{L"AC_HandleSelfFileIntegrityResultPacket", NetcSignatures::ScanModuleIntegrity,
            reinterpret_cast<void*>(&HookScanModuleIntegrity)},
        HookUtils::Hook{L"CNet__SendPacket", NetcSignatures::SendPacket,
            reinterpret_cast<void*>(&HookSendPacket), reinterpret_cast<void**>(&g_sendPacket)}
    };
    g_installed = HookUtils::Install(scanner, hooks);
    if (!g_installed)
        g_sendPacket = nullptr;
    ReleaseSRWLockExclusive(&g_installLock);
    return g_installed;
}
