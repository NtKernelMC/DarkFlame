#include "text_display_hook.h"

#include "gui.h"
#include "logger.h"
#include "lua_bridge.h"
#include "signature_scanner.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
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
std::atomic_bool g_installed{};

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
    if(g_installed.load(std::memory_order_acquire))
        return true;
    if(!client)
        return false;

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

    g_installed.store(true, std::memory_order_release);
    Log::Scan(L"TextDisplaySetCaption", L"hook_installed",
        reinterpret_cast<std::uintptr_t>(g_target));
    return true;
}
