#include "plugin.h"

#include "fairplaykd.h"

#include <TlHelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{

enum MenuEntry
{
    MenuStatus = 1,
    MenuRetry
};

enum class RunState
{
    Starting,
    Active,
    ActiveWithWarnings,
    Failed,
    Stopped
};

struct RuntimeStatus
{
    RunState state = RunState::Starting;
    DWORD lastError = ERROR_SUCCESS;
    std::uint32_t driverMagic = 0;
    DWORD gtaProcessId = 0;
    bool gtaReadSucceeded = false;
    bool blobSucceeded = false;
    bool ntoskrnlSucceeded = false;
    fpkd::FpkdBlob blob{};
    fpkd::FpkdModuleInfo ntoskrnl{};
    char detail[256] = "Waiting for initialization";
};

SRWLOCK statusLock = SRWLOCK_INIT;
RuntimeStatus runtimeStatus;
HANDLE stopEvent = nullptr;
HANDLE retryEvent = nullptr;
HANDLE workerThread = nullptr;

struct GtaReadResult
{
    DWORD processId = 0;
    void* imageBase = nullptr;
    unsigned char bytes[16]{};
};

bool FindProcessId(const wchar_t* imageName, DWORD* processId)
{
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if(::Process32FirstW(snapshot, &entry))
    {
        do
        {
            if(::_wcsicmp(entry.szExeFile, imageName) == 0)
            {
                *processId = entry.th32ProcessID;
                found = true;
                break;
            }
        } while(::Process32NextW(snapshot, &entry));
    }

    const DWORD error = found ? ERROR_SUCCESS : ERROR_NOT_FOUND;
    ::CloseHandle(snapshot);
    ::SetLastError(error);
    return found;
}

bool FindMainModule(DWORD processId, MODULEENTRY32W* module)
{
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for(int attempt = 0; attempt < 8; ++attempt)
    {
        snapshot = ::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if(snapshot != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_BAD_LENGTH)
            break;
    }
    if(snapshot == INVALID_HANDLE_VALUE)
        return false;

    module->dwSize = sizeof(*module);
    const BOOL found = ::Module32FirstW(snapshot, module);
    const DWORD error = found ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(snapshot);
    ::SetLastError(error);
    return found != FALSE;
}

bool TestGtaReadProcessMemory(GtaReadResult* result)
{
    if(!FindProcessId(L"gta_sa.exe", &result->processId))
        return false;

    MODULEENTRY32W module{};
    if(!FindMainModule(result->processId, &module))
        return false;
    result->imageBase = module.modBaseAddr;

    const HANDLE process = ::OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        result->processId);
    if(!process)
        return false;

    SIZE_T bytesRead = 0;
    const BOOL read = ::ReadProcessMemory(
        process,
        module.modBaseAddr,
        result->bytes,
        sizeof(result->bytes),
        &bytesRead);
    const DWORD error = read ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(process);

    if(!read || bytesRead != sizeof(result->bytes))
    {
        ::SetLastError(read ? ERROR_PARTIAL_COPY : error);
        return false;
    }

    if(result->bytes[0] != 'M' || result->bytes[1] != 'Z')
    {
        ::SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }
    return true;
}

void Log(const char* format, ...)
{
    char message[1024]{};
    va_list args;
    va_start(args, format);
    ::vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    _plugin_logprintf("[Sirenhead] %s\n", message);
}

const char* StateName(RunState state)
{
    switch(state)
    {
    case RunState::Starting:
        return "starting";
    case RunState::Active:
        return "active";
    case RunState::ActiveWithWarnings:
        return "active with warnings";
    case RunState::Failed:
        return "failed";
    case RunState::Stopped:
        return "stopped";
    }
    return "unknown";
}

RuntimeStatus SnapshotStatus()
{
    ::AcquireSRWLockShared(&statusLock);
    const RuntimeStatus snapshot = runtimeStatus;
    ::ReleaseSRWLockShared(&statusLock);
    return snapshot;
}

void StoreStatus(const RuntimeStatus& status)
{
    ::AcquireSRWLockExclusive(&statusLock);
    runtimeStatus = status;
    ::ReleaseSRWLockExclusive(&statusLock);
}

void SetDetail(RuntimeStatus& status, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ::vsnprintf_s(status.detail, sizeof(status.detail), _TRUNCATE, format, args);
    va_end(args);
}

void LogWindowsError(const char* operation, DWORD error)
{
    const std::string message = fpkd::LastErrorString(error);
    Log("[error] %s failed: %lu (0x%08lX): %s",
        operation, error, error, message.c_str());
}

void PrintStatus()
{
    const RuntimeStatus status = SnapshotStatus();
    Log("state=%s; detail=%s", StateName(status.state), status.detail);

    if(status.driverMagic)
        Log("driver magic=%u; x32dbg PID=%lu",
            status.driverMagic, ::GetCurrentProcessId());
    if(status.gtaReadSucceeded)
        Log("gta_sa.exe PID=%lu ReadProcessMemory test passed", status.gtaProcessId);
    if(status.blobSucceeded)
        Log("blob: magic=%u count=%u flags=%u enabled=%u",
            status.blob.magic0,
            status.blob.count,
            status.blob.flags,
            static_cast<unsigned>(status.blob.enabled));
    if(status.ntoskrnlSucceeded)
        Log("ntoskrnl export address=0x%llX image size=0x%llX",
            static_cast<unsigned long long>(status.ntoskrnl.exportAddr),
            static_cast<unsigned long long>(status.ntoskrnl.moduleSize));
}

void RunWorkflow(fpkd::FairplayKd& driver)
{
    RuntimeStatus status{};
    status.state = RunState::Starting;
    SetDetail(status, "Authenticating x32dbg with FairplayKD0");
    StoreStatus(status);

    Log("initializing for x32dbg PID=%lu", ::GetCurrentProcessId());

    if(!driver.Handshake())
    {
        status.state = RunState::Failed;
        status.lastError = driver.GetLastError();
        SetDetail(status, "FairplayKD handshake/callback activation failed");
        StoreStatus(status);
        LogWindowsError("handshake + callback activation", status.lastError);
        if(status.lastError == ERROR_ACCESS_DENIED)
            Log("start x32dbg as administrator and retry");
        return;
    }

    status.driverMagic = fpkd::kMagic;
    status.state = RunState::Active;
    SetDetail(status, "Callback policies 5 and 6 are active for x32dbg");
    StoreStatus(status);
    Log("authenticated; callback policies 5 and 6 enabled for PID %lu",
        ::GetCurrentProcessId());

    bool warnings = false;
    GtaReadResult readResult{};
    if(TestGtaReadProcessMemory(&readResult))
    {
        status.gtaReadSucceeded = true;
        status.gtaProcessId = readResult.processId;

        char bytes[16 * 3 + 1]{};
        std::size_t offset = 0;
        for(const std::uint8_t byte : readResult.bytes)
        {
            offset += static_cast<std::size_t>(::sprintf_s(
                bytes + offset, sizeof(bytes) - offset, "%02X ", byte));
        }
        if(offset)
            bytes[offset - 1] = '\0';

        Log("gta_sa.exe PID=%lu base=%p ReadProcessMemory: %s",
            readResult.processId, readResult.imageBase, bytes);
    }
    else
    {
        warnings = true;
        status.lastError = ::GetLastError();
        LogWindowsError("gta_sa.exe ReadProcessMemory test", status.lastError);
    }

    if(driver.QueryBlob(&status.blob))
    {
        status.blobSucceeded = true;
        Log("driver magic=%u count=%u flags=%u enabled=%u",
            status.blob.magic0,
            status.blob.count,
            status.blob.flags,
            static_cast<unsigned>(status.blob.enabled));
    }
    else
    {
        warnings = true;
        status.lastError = driver.GetLastError();
        LogWindowsError("QueryBlob", status.lastError);
    }

    if(driver.GetNtoskrnlInfo(&status.ntoskrnl))
    {
        status.ntoskrnlSucceeded = true;
        Log("ntoskrnl export address=0x%llX image size=0x%llX",
            static_cast<unsigned long long>(status.ntoskrnl.exportAddr),
            static_cast<unsigned long long>(status.ntoskrnl.moduleSize));
    }
    else
    {
        warnings = true;
        status.lastError = driver.GetLastError();
        LogWindowsError("GetNtoskrnlInfo", status.lastError);
    }

    status.state = warnings ? RunState::ActiveWithWarnings : RunState::Active;
    SetDetail(
        status,
        warnings
            ? "Callback policies are active; one or more diagnostics failed"
            : "All WildCoyotte operations completed successfully");
    StoreStatus(status);
}

DWORD WINAPI WorkerMain(void*)
{
    fpkd::FairplayKd driver;
    for(;;)
    {
        RunWorkflow(driver);

        const HANDLE events[] = {stopEvent, retryEvent};
        const DWORD wait = ::WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if(wait == WAIT_OBJECT_0)
            break;
        if(wait != WAIT_OBJECT_0 + 1)
        {
            LogWindowsError("WaitForMultipleObjects", ::GetLastError());
            break;
        }
    }

    driver.Unregister();

    RuntimeStatus status = SnapshotStatus();
    status.state = RunState::Stopped;
    SetDetail(status, "Worker stopped");
    StoreStatus(status);
    return 0;
}

void RequestRetry()
{
    if(retryEvent)
        ::SetEvent(retryEvent);
}

bool CommandStatus(int, char**)
{
    PrintStatus();
    return true;
}

bool CommandRetry(int, char**)
{
    Log("retry requested");
    RequestRetry();
    return true;
}

void CallbackMenuEntry(CBTYPE, void* callbackInfo)
{
    const auto* info = static_cast<PLUG_CB_MENUENTRY*>(callbackInfo);
    if(!info)
        return;

    switch(info->hEntry)
    {
    case MenuStatus:
        PrintStatus();
        break;
    case MenuRetry:
        Log("retry requested from menu");
        RequestRetry();
        break;
    default:
        break;
    }
}

void CallbackDebugLifecycle(CBTYPE type, void*)
{
    if(type == CB_INITDEBUG || type == CB_CREATEPROCESS || type == CB_ATTACH)
        RequestRetry();
}

}

bool pluginInit(PLUG_INITSTRUCT*)
{
    stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    retryEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(!stopEvent || !retryEvent)
    {
        LogWindowsError("CreateEvent", ::GetLastError());
        if(stopEvent)
            ::CloseHandle(stopEvent);
        if(retryEvent)
            ::CloseHandle(retryEvent);
        stopEvent = nullptr;
        retryEvent = nullptr;
        return false;
    }

    _plugin_registercallback(pluginHandle, CB_MENUENTRY, CallbackMenuEntry);
    _plugin_registercallback(pluginHandle, CB_INITDEBUG, CallbackDebugLifecycle);
    _plugin_registercallback(pluginHandle, CB_CREATEPROCESS, CallbackDebugLifecycle);
    _plugin_registercallback(pluginHandle, CB_ATTACH, CallbackDebugLifecycle);
    _plugin_registercommand(pluginHandle, "sirenhead.status", CommandStatus, true);
    _plugin_registercommand(pluginHandle, "sirenhead.retry", CommandRetry, true);

    workerThread = ::CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    if(!workerThread)
    {
        LogWindowsError("CreateThread", ::GetLastError());
        _plugin_unregistercommand(pluginHandle, "sirenhead.status");
        _plugin_unregistercommand(pluginHandle, "sirenhead.retry");
        _plugin_unregistercallback(pluginHandle, CB_ATTACH);
        _plugin_unregistercallback(pluginHandle, CB_CREATEPROCESS);
        _plugin_unregistercallback(pluginHandle, CB_INITDEBUG);
        _plugin_unregistercallback(pluginHandle, CB_MENUENTRY);
        ::CloseHandle(retryEvent);
        ::CloseHandle(stopEvent);
        retryEvent = nullptr;
        stopEvent = nullptr;
        return false;
    }

    Log("plugin loaded; automatic initialization queued");
    return true;
}

void pluginStop()
{
    _plugin_unregistercommand(pluginHandle, "sirenhead.status");
    _plugin_unregistercommand(pluginHandle, "sirenhead.retry");
    _plugin_unregistercallback(pluginHandle, CB_ATTACH);
    _plugin_unregistercallback(pluginHandle, CB_CREATEPROCESS);
    _plugin_unregistercallback(pluginHandle, CB_INITDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_MENUENTRY);

    if(stopEvent)
        ::SetEvent(stopEvent);
    if(workerThread)
    {
        ::WaitForSingleObject(workerThread, INFINITE);
        ::CloseHandle(workerThread);
        workerThread = nullptr;
    }
    if(retryEvent)
    {
        ::CloseHandle(retryEvent);
        retryEvent = nullptr;
    }
    if(stopEvent)
    {
        ::CloseHandle(stopEvent);
        stopEvent = nullptr;
    }

    Log("plugin stopped");
}

void pluginSetup()
{
    _plugin_menuaddentry(hMenu, MenuStatus, "Status");
    _plugin_menuaddentry(hMenu, MenuRetry, "Retry now");
}
