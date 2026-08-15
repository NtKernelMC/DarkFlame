#include "forensic_hooks.h"

#include "logger.h"

#include <MinHook.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
struct ExportHook
{
    const wchar_t* module{};
    const char* name{};
    void* detour{};
    void** original{};
    void* target{};
};

using Process32FirstWFn = BOOL(WINAPI*)(HANDLE, LPPROCESSENTRY32W);
using Process32NextWFn = BOOL(WINAPI*)(HANDLE, LPPROCESSENTRY32W);
using Process32FirstAFn = BOOL(WINAPI*)(HANDLE, tagPROCESSENTRY32*);
using Process32NextAFn = BOOL(WINAPI*)(HANDLE, tagPROCESSENTRY32*);
using EnumProcessesFn = BOOL(WINAPI*)(DWORD*, DWORD, DWORD*);
using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);

std::atomic_bool g_started{};
Process32FirstWFn g_process32FirstW{};
Process32NextWFn g_process32NextW{};
Process32FirstAFn g_process32FirstA{};
Process32NextAFn g_process32NextA{};
EnumProcessesFn g_k32EnumProcesses{};
EnumProcessesFn g_enumProcesses{};
OpenProcessFn g_openProcess{};

bool ContainsI(std::wstring_view text, std::wstring_view needle)
{
    if(needle.empty() || text.size() < needle.size())
        return false;
    for(std::size_t offset = 0; offset <= text.size() - needle.size(); ++offset)
    {
        if(!_wcsnicmp(text.data() + offset, needle.data(), needle.size()))
            return true;
    }
    return false;
}

std::wstring Wide(const char* value)
{
    if(!value || !*value)
        return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if(size <= 1)
        return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, output.data(), size);
    output.resize(static_cast<std::size_t>(size - 1));
    return output;
}

bool HiddenProcessName(std::wstring_view input)
{
    const std::size_t separator = input.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos
        ? input : input.substr(separator + 1);
    const std::wstring owned(name);
    if(!_wcsicmp(owned.c_str(), L"DarkFlame.exe")
        || !_wcsicmp(owned.c_str(), L"devenv.exe")
        || !_wcsicmp(owned.c_str(), L"MSBuild.exe")
        || !_wcsicmp(owned.c_str(), L"VBCSCompiler.exe")
        || !_wcsicmp(owned.c_str(), L"VSIXInstaller.exe")
        || !_wcsicmp(owned.c_str(), L"PerfWatson2.exe")
        || !_wcsicmp(owned.c_str(), L"vsjitdebugger.exe")
        || !_wcsicmp(owned.c_str(), L"vsdebugconsole.exe"))
        return true;
    return ContainsI(name, L"SERVICEHUB.")
        || ContainsI(name, L"MICROSOFT.VISUALSTUDIO.")
        || ContainsI(name, L"VSSTANDARDCOLLECTOR")
        || ContainsI(name, L"VSTEST.");
}

bool HiddenProcessId(DWORD pid)
{
    if(!pid || !g_openProcess)
        return false;
    HANDLE process = g_openProcess(PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if(!process)
        return false;
    wchar_t path[32768]{};
    DWORD size = std::size(path);
    const bool hidden = QueryFullProcessImageNameW(process, 0, path, &size)
        && HiddenProcessName(std::wstring_view(path, size));
    CloseHandle(process);
    return hidden;
}

__declspec(noinline) BOOL WINAPI HookProcess32FirstW(HANDLE snapshot,
    LPPROCESSENTRY32W entry)
{
    BOOL result = g_process32FirstW(snapshot, entry);
    while(result && entry && HiddenProcessName(entry->szExeFile))
        result = g_process32NextW(snapshot, entry);
    return result;
}

__declspec(noinline) BOOL WINAPI HookProcess32NextW(HANDLE snapshot,
    LPPROCESSENTRY32W entry)
{
    BOOL result = g_process32NextW(snapshot, entry);
    while(result && entry && HiddenProcessName(entry->szExeFile))
        result = g_process32NextW(snapshot, entry);
    return result;
}

__declspec(noinline) BOOL WINAPI HookProcess32FirstA(HANDLE snapshot,
    tagPROCESSENTRY32* entry)
{
    BOOL result = g_process32FirstA(snapshot, entry);
    while(result && entry && HiddenProcessName(Wide(entry->szExeFile)))
        result = g_process32NextA(snapshot, entry);
    return result;
}

__declspec(noinline) BOOL WINAPI HookProcess32NextA(HANDLE snapshot,
    tagPROCESSENTRY32* entry)
{
    BOOL result = g_process32NextA(snapshot, entry);
    while(result && entry && HiddenProcessName(Wide(entry->szExeFile)))
        result = g_process32NextA(snapshot, entry);
    return result;
}

void FilterPids(DWORD* pids, DWORD capacity, DWORD* needed)
{
    if(!pids || !needed)
        return;
    const DWORD count = std::min(*needed, capacity) / sizeof(DWORD);
    DWORD output{};
    for(DWORD index = 0; index < count; ++index)
    {
        if(!HiddenProcessId(pids[index]))
            pids[output++] = pids[index];
    }
    *needed = output * sizeof(DWORD);
}

__declspec(noinline) BOOL WINAPI HookK32EnumProcesses(DWORD* pids,
    DWORD capacity, DWORD* needed)
{
    const BOOL result = g_k32EnumProcesses(pids, capacity, needed);
    if(result)
        FilterPids(pids, capacity, needed);
    return result;
}

__declspec(noinline) BOOL WINAPI HookEnumProcesses(DWORD* pids,
    DWORD capacity, DWORD* needed)
{
    const BOOL result = g_enumProcesses(pids, capacity, needed);
    if(result)
        FilterPids(pids, capacity, needed);
    return result;
}

__declspec(noinline) HANDLE WINAPI HookOpenProcess(DWORD access,
    BOOL inherit, DWORD pid)
{
    if(HiddenProcessId(pid))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    return g_openProcess(access, inherit, pid);
}

bool InstallHooks()
{
    std::vector<ExportHook> hooks{
        {L"kernel32.dll", "OpenProcess", &HookOpenProcess,
            reinterpret_cast<void**>(&g_openProcess)},
        {L"kernel32.dll", "Process32FirstW", &HookProcess32FirstW,
            reinterpret_cast<void**>(&g_process32FirstW)},
        {L"kernel32.dll", "Process32NextW", &HookProcess32NextW,
            reinterpret_cast<void**>(&g_process32NextW)},
        {L"kernel32.dll", "Process32First", &HookProcess32FirstA,
            reinterpret_cast<void**>(&g_process32FirstA)},
        {L"kernel32.dll", "Process32Next", &HookProcess32NextA,
            reinterpret_cast<void**>(&g_process32NextA)},
        {L"kernel32.dll", "K32EnumProcesses", &HookK32EnumProcesses,
            reinterpret_cast<void**>(&g_k32EnumProcesses)},
        {L"psapi.dll", "EnumProcesses", &HookEnumProcesses,
            reinterpret_cast<void**>(&g_enumProcesses)}
    };

    std::unordered_set<void*> targets;
    std::size_t enabled{};
    for(ExportHook& hook : hooks)
    {
        HMODULE module = GetModuleHandleW(hook.module);
        if(!module)
            module = LoadLibraryW(hook.module);
        hook.target = module
            ? reinterpret_cast<void*>(GetProcAddress(module, hook.name)) : nullptr;
        if(!hook.target || !targets.insert(hook.target).second)
            continue;
        if(MH_CreateHook(hook.target, hook.detour, hook.original) != MH_OK)
            continue;
        if(MH_EnableHook(hook.target) != MH_OK)
        {
            MH_RemoveHook(hook.target);
            continue;
        }
        ++enabled;
    }
    wchar_t message[96]{};
    swprintf_s(message, L"[process-filter] installed %zu process hooks", enabled);
    Log::Write(message);
    return enabled != 0;
}

DWORD WINAPI InstallThread(void*)
{
    if(!InstallHooks())
        Log::Write(L"[process-filter] hook installation failed");
    return 0;
}
}

bool StartForensicHooks(HMODULE, std::wstring_view)
{
    if(g_started.exchange(true))
        return false;
    HANDLE thread = CreateThread(nullptr, 0, &InstallThread, nullptr, 0, nullptr);
    if(!thread)
        return false;
    CloseHandle(thread);
    return true;
}
