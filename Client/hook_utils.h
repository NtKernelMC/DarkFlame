#pragma once

#include "logger.h"
#include "signature_scanner.h"

#include <MinHook.h>

#include <span>
#include <string>
#include <string_view>

namespace HookUtils
{
struct Hook
{
    std::wstring_view name;
    std::string_view pattern;
    void* detour{};
    void** original{};
    void* target{};
};

inline std::wstring Failure(std::wstring_view operation, MH_STATUS status)
{
    wchar_t buffer[96]{};
    swprintf_s(buffer, L"%.*ls_failed (MinHook=%d)",
        static_cast<int>(operation.size()), operation.data(), static_cast<int>(status));
    return buffer;
}

inline void Remove(std::span<Hook> hooks)
{
    for (Hook& hook : hooks)
    {
        if (!hook.target)
            continue;
        MH_DisableHook(hook.target);
        MH_RemoveHook(hook.target);
        if (hook.original)
            *hook.original = nullptr;
    }
}

inline bool Resolve(const SignatureScanner& scanner, Hook& hook)
{
    Log::Scan(hook.name, L"searching");
    hook.target = reinterpret_cast<void*>(scanner.Find(hook.pattern));
    if (!hook.target)
    {
        Log::Scan(hook.name, L"not_found");
        return false;
    }
    Log::Scan(hook.name, L"found", reinterpret_cast<std::uintptr_t>(hook.target));
    return true;
}

inline bool Install(const SignatureScanner& scanner, std::span<Hook> hooks)
{
    for (Hook& hook : hooks)
    {
        if (!Resolve(scanner, hook))
            return false;
    }

    std::size_t created{};
    for (Hook& hook : hooks)
    {
        const MH_STATUS status = MH_CreateHook(hook.target, hook.detour, hook.original);
        if (status != MH_OK)
        {
            Log::Scan(hook.name, Failure(L"hook_create", status),
                reinterpret_cast<std::uintptr_t>(hook.target));
            Remove(hooks.first(created));
            return false;
        }
        ++created;
    }

    for (Hook& hook : hooks)
    {
        const MH_STATUS status = MH_QueueEnableHook(hook.target);
        if (status != MH_OK)
        {
            Log::Scan(hook.name, Failure(L"hook_queue", status),
                reinterpret_cast<std::uintptr_t>(hook.target));
            Remove(hooks.first(created));
            return false;
        }
    }

    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK)
    {
        Log::Write(Failure(L"hook_apply", applied));
        Remove(hooks.first(created));
        return false;
    }

    for (const Hook& hook : hooks)
        Log::Scan(hook.name, L"hook_installed", reinterpret_cast<std::uintptr_t>(hook.target));
    return true;
}
}
