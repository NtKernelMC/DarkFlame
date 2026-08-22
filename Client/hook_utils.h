#pragma once

#include "logger.h"
#include "signature_scanner.h"

#include <MinHook.h>

#include <array>
#include <cstring>
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
    std::array<unsigned char, 8> patch{};
    bool patchValid{};
};

inline bool ReadPatch(Hook& hook)
{
    if(!hook.target)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(hook.target, &info, sizeof(info)) || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    __try
    {
        std::memcpy(hook.patch.data(), hook.target, hook.patch.size());
        hook.patchValid = true;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        hook.patchValid = false;
        return false;
    }
}

inline bool PatchIntact(const Hook& hook)
{
    if(!hook.target || !hook.patchValid)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(hook.target, &info, sizeof(info)) || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    __try
    {
        return !std::memcmp(hook.patch.data(), hook.target, hook.patch.size());
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

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
        hook.target = nullptr;
        hook.patchValid = false;
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
            Remove(hooks.first(created + 1));
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

    for (Hook& hook : hooks)
    {
        ReadPatch(hook);
        Log::Scan(hook.name, L"hook_installed", reinterpret_cast<std::uintptr_t>(hook.target));
    }
    return true;
}

inline bool Repair(std::span<Hook> hooks)
{
    for(Hook& hook : hooks)
    {
        if(PatchIntact(hook))
            continue;
        const MH_STATUS disabled = MH_DisableHook(hook.target);
        if(disabled != MH_OK && disabled != MH_ERROR_DISABLED)
        {
            Log::Scan(hook.name, Failure(L"hook_repair_disable", disabled),
                reinterpret_cast<std::uintptr_t>(hook.target));
            return false;
        }
        const MH_STATUS enabled = MH_EnableHook(hook.target);
        if(enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        {
            Log::Scan(hook.name, Failure(L"hook_repair_enable", enabled),
                reinterpret_cast<std::uintptr_t>(hook.target));
            return false;
        }
        if(!ReadPatch(hook))
            return false;
        Log::Scan(hook.name, L"hook_restored",
            reinterpret_cast<std::uintptr_t>(hook.target));
    }
    return true;
}
}
