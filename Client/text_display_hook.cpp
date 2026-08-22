#include "text_display_hook.h"

#include "gui.h"
#include "logger.h"
#include "lua_bridge.h"
#include "signature_scanner.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view TextDisplaySetCaptionPattern =
    "55 8B EC 83 EC 24 A1 ? ? ? ? 33 C5 89 45 FC 53 8B 5D 08 8B C1 89 45 DC "
    "89 5D E0 85 DB 0F 84 3C 01 00 00";

using TextDisplaySetCaptionFn = void(__thiscall*)(void*, const char*);

TextDisplaySetCaptionFn g_textDisplaySetCaption{};
void* g_target{};
HMODULE g_module{};
std::array<unsigned char, 8> g_patch{};
bool g_patchValid{};
std::atomic_bool g_installed{};
std::mutex g_hookMutex;

bool ReadPatch(std::array<unsigned char, 8>& patch)
{
    if(!g_target)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(g_target, &memory, sizeof(memory))
        || memory.State != MEM_COMMIT
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    {
        return false;
    }
    __try
    {
        std::memcpy(patch.data(), g_target, patch.size());
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool RepairTextDisplayHook()
{
    std::array<unsigned char, 8> current{};
    if(g_patchValid && ReadPatch(current) && current == g_patch)
        return true;
    const MH_STATUS disabled = MH_DisableHook(g_target);
    if(disabled != MH_OK && disabled != MH_ERROR_DISABLED)
        return false;
    const MH_STATUS enabled = MH_EnableHook(g_target);
    if(enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        return false;
    g_patchValid = ReadPatch(g_patch);
    if(g_patchValid)
        Log::Write(L"[trambot] restored TextDisplaySetCaption hook");
    return g_patchValid;
}

void RemoveTextDisplayHook()
{
    if(g_target)
    {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
    }
    g_textDisplaySetCaption = nullptr;
    g_target = nullptr;
    g_module = nullptr;
    g_patch = {};
    g_patchValid = false;
    g_installed.store(false, std::memory_order_release);
}

bool ContainsI(std::string_view text, std::string_view needle)
{
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
        [](unsigned char left, unsigned char right)
        {
            return std::toupper(left) == std::toupper(right);
        }) != text.end();
}

void __fastcall HookTextDisplaySetCaption(void* self, void*, const char* caption)
{
    g_textDisplaySetCaption(self, caption);
    if(!caption || !GuiTramBotEnabled())
        return;

    std::string message(caption);
    if(!ContainsI(message, "(ADMIN)"))
        return;
    std::replace_if(message.begin(), message.end(), [](char value)
    {
        return value == '\r' || value == '\n';
    }, ' ');

    GuiQueueTramAdminCaption(std::move(message));
    if(GuiTramSirenEnabled())
        PlayTramAlertSignal();
    Log::Write(L"[trambot] admin caption detected");
}
}

bool InstallTextDisplayHook(HMODULE client)
{
    std::scoped_lock lock(g_hookMutex);
    if(g_installed.load(std::memory_order_acquire) && g_module == client)
        return RepairTextDisplayHook();
    if(!client)
        return false;
    if(g_installed.load(std::memory_order_acquire))
        RemoveTextDisplayHook();

    g_target = reinterpret_cast<void*>(
        SignatureScanner(client).Find(TextDisplaySetCaptionPattern));
    Log::Scan(L"TextDisplaySetCaption", g_target ? L"found" : L"not_found",
        reinterpret_cast<std::uintptr_t>(g_target));
    if(!g_target)
        return false;

    const MH_STATUS created = MH_CreateHook(g_target,
        reinterpret_cast<void*>(&HookTextDisplaySetCaption),
        reinterpret_cast<void**>(&g_textDisplaySetCaption));
    if(created != MH_OK)
    {
        Log::Scan(L"TextDisplaySetCaption", L"hook_create_failed",
            reinterpret_cast<std::uintptr_t>(g_target));
        g_target = nullptr;
        return false;
    }
    if(MH_EnableHook(g_target) != MH_OK)
    {
        MH_RemoveHook(g_target);
        g_textDisplaySetCaption = nullptr;
        g_target = nullptr;
        Log::Write(L"[trambot] TextDisplaySetCaption hook enable failed");
        return false;
    }

    g_module = client;
    g_patchValid = ReadPatch(g_patch);
    g_installed.store(true, std::memory_order_release);
    Log::Scan(L"TextDisplaySetCaption", L"hook_installed",
        reinterpret_cast<std::uintptr_t>(g_target));
    return true;
}

void ResetTextDisplayHook(HMODULE client)
{
    std::scoped_lock lock(g_hookMutex);
    if(g_installed.load(std::memory_order_acquire)
        && (!client || g_module == client))
    {
        RemoveTextDisplayHook();
    }
}
