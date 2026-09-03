#pragma once

#include "bootstrap_protocol.h"
#include "environment.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iterator>
#include <new.h>
#include <string>
#include <string_view>

#pragma comment(lib, "Dbghelp.lib")

namespace CrashHandler
{
#if defined(_M_IX86)
inline HANDLE g_process{};
inline DWORD64 g_imageBase{};
inline DWORD g_imageSize{};
inline bool g_symbolsReady{};
inline bool g_installed{};
inline LONG g_reporting{};
inline LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter{};
inline PVOID g_vectoredHandler{};
inline _invalid_parameter_handler g_previousInvalidParameter{};
inline _purecall_handler g_previousPureCall{};
inline std::terminate_handler g_previousTerminate{};
inline void(__cdecl* g_previousAbort)(int) = SIG_DFL;
inline BYTE* g_filterTarget{};
inline BYTE g_filterBackup[5]{};
inline bool g_filterPatched{};
inline wchar_t g_imagePath[1024]{};
inline wchar_t g_artifactDirectory[1024]{};
inline wchar_t g_moduleName[64]{};
inline char g_moduleNameAnsi[64]{};

inline DWORD ReadImageSize(HMODULE image)
{
    if (!image)
        return 0;
    const auto* base = reinterpret_cast<const BYTE*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
}

template<std::size_t Size>
inline bool Copy(wchar_t (&destination)[Size], std::wstring_view source)
{
    if (source.empty() || source.size() >= Size)
        return false;
    std::wmemcpy(destination, source.data(), source.size());
    destination[source.size()] = L'\0';
    return true;
}

inline void BuildArtifactDirectory(std::wstring_view preferredDirectory)
{
    std::wstring directory(preferredDirectory);
    if(directory.empty())
        directory = Environment::Read(BootstrapProtocol::LogDirectoryVariable);
    if (!directory.empty())
    {
        swprintf_s(g_artifactDirectory, L"%ls\\CrashDumps", directory.c_str());
        CreateDirectoryW(g_artifactDirectory, nullptr);
        return;
    }

    wchar_t base[1024]{};
    if(g_imagePath[0])
        wcscpy_s(base, g_imagePath);
    else
        GetModuleFileNameW(nullptr, base, static_cast<DWORD>(std::size(base)));

    if (wchar_t* separator = std::wcsrchr(base, L'\\'))
        *separator = L'\0';
    swprintf_s(g_artifactDirectory, L"%ls\\CrashDumps", base[0] ? base : L".");
    CreateDirectoryW(g_artifactDirectory, nullptr);
}

inline void ClearOldArtifacts()
{
    wchar_t pattern[1200]{};
    swprintf_s(pattern, L"%ls\\%lsCrash_*", g_artifactDirectory, g_moduleName);
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const wchar_t* extension = std::wcsrchr(data.cFileName, L'.');
        if (!extension || (_wcsicmp(extension, L".log") && _wcsicmp(extension, L".dmp")))
            continue;
        wchar_t path[1200]{};
        swprintf_s(path, L"%ls\\%ls", g_artifactDirectory, data.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

inline void Write(HANDLE file, const char* format, ...)
{
    if (file == INVALID_HANDLE_VALUE || !format)
        return;
    char text[2048]{};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
    va_end(arguments);
    DWORD written{};
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

inline const char* ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC00001A5u: return "STATUS_INVALID_EXCEPTION_HANDLER";
    case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000602u: return "STATUS_FAIL_FAST_EXCEPTION";
    case 0xE06D7363u: return "MSVC_CPP_EXCEPTION";
    default: return "UNKNOWN_EXCEPTION";
    }
}

inline bool IsInMappedImage(DWORD64 address)
{
    return g_imageBase && g_imageSize
        && address >= g_imageBase && address < g_imageBase + g_imageSize;
}

inline void BuildArtifactPath(wchar_t* output, std::size_t size, const wchar_t* extension)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    swprintf_s(output, size,
        L"%ls\\%lsCrash_%04u%02u%02u_%02u%02u%02u_%03u_P%08lX_T%08lX.%ls",
        g_artifactDirectory, g_moduleName,
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId(), extension);
}

inline bool LoadSymbols()
{
    g_process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
        | SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(g_process, nullptr, TRUE))
        return false;

    wchar_t searchPath[1024]{};
    wcscpy_s(searchPath, g_imagePath);
    if (wchar_t* separator = std::wcsrchr(searchPath, L'\\'))
    {
        *separator = L'\0';
        SymSetSearchPathW(g_process, searchPath);
    }

    const DWORD64 loaded = SymLoadModuleExW(g_process, nullptr, g_imagePath,
        g_moduleName, g_imageBase, g_imageSize, nullptr, 0);
    IMAGEHLP_MODULEW64 module{};
    module.SizeOfStruct = sizeof(module);
    const bool registered = loaded == g_imageBase
        || SymGetModuleInfoW64(g_process, g_imageBase, &module) == TRUE;
    if (!registered)
        return false;

    const auto* base = reinterpret_cast<const BYTE*>(
        static_cast<std::uintptr_t>(g_imageBase));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        base + dos->e_lfanew);
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement{};
    SymFromAddr(g_process, g_imageBase
        + nt->OptionalHeader.AddressOfEntryPoint, &displacement, symbol);

    module = {};
    module.SizeOfStruct = sizeof(module);
    return SymGetModuleInfoW64(g_process, g_imageBase, &module)
        && (module.SymType == SymPdb || module.LoadedPdbName[0]);
}

inline void DescribeAddress(HANDLE log, unsigned index, DWORD64 address, bool returnAddress)
{
    const DWORD64 lookup = returnAddress && address ? address - 1 : address;
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement{};
    const bool hasSymbol = g_symbolsReady
        && SymFromAddr(g_process, lookup, &displacement, symbol);
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement{};
    const bool hasLine = g_symbolsReady
        && SymGetLineFromAddr64(g_process, lookup, &lineDisplacement, &line)
        && line.FileName;

    char module[MAX_PATH]{};
    if (IsInMappedImage(lookup))
        strcpy_s(module, g_moduleNameAnsi);
    else if (g_symbolsReady)
    {
        const DWORD64 base = SymGetModuleBase64(g_process, lookup);
        char path[MAX_PATH]{};
        if (base && GetModuleFileNameA(reinterpret_cast<HMODULE>(base), path, MAX_PATH))
        {
            const char* separator = std::strrchr(path, '\\');
            strcpy_s(module, separator ? separator + 1 : path);
        }
    }
    if (!module[0])
        strcpy_s(module, "unknown");

    Write(log, "  #%02u  0x%08llX  %s", index,
        static_cast<unsigned long long>(address), module);
    if (hasSymbol)
        Write(log, "!%s+0x%llX", symbol->Name,
            static_cast<unsigned long long>(displacement));
    else if (IsInMappedImage(lookup))
        Write(log, "+0x%08llX", static_cast<unsigned long long>(lookup - g_imageBase));
    if (hasLine)
        Write(log, "  [%s:%lu]", line.FileName, line.LineNumber);
    Write(log, "\r\n");
}

inline void WriteStack(HANDLE log, const CONTEXT& source)
{
    CONTEXT context = source;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    DescribeAddress(log, 0, context.Eip, false);
    DWORD64 previous = context.Eip;
    unsigned written = 1;
    for (unsigned attempts = 0; attempts < 128 && written < 64; ++attempts)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, g_process, GetCurrentThread(),
            &frame, &context, nullptr, SymFunctionTableAccess64,
            SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
        {
            break;
        }
        if (frame.AddrPC.Offset == previous)
            continue;
        DescribeAddress(log, written++, frame.AddrPC.Offset, true);
        previous = frame.AddrPC.Offset;
    }
}

inline void WriteMiniDump(PEXCEPTION_POINTERS exception)
{
    wchar_t path[1200]{};
    BuildArtifactPath(path, std::size(path), L"dmp");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = exception;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs
        | MiniDumpWithHandleData | MiniDumpWithUnloadedModules
        | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory
        | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo
        | MiniDumpIgnoreInaccessibleMemory);
    if (!MiniDumpWriteDump(g_process, GetCurrentProcessId(), file, type,
        &information, nullptr, nullptr))
    {
        CloseHandle(file);
        DeleteFileW(path);
        return;
    }
    CloseHandle(file);
}

inline LONG WriteReport(const char* reason, PEXCEPTION_POINTERS exception)
{
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord
        || InterlockedCompareExchange(&g_reporting, 1, 0))
    {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    wchar_t path[1200]{};
    BuildArtifactPath(path, std::size(path), L"log");
    HANDLE log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        return EXCEPTION_EXECUTE_HANDLER;

    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    const auto& context = *exception->ContextRecord;
    Write(log, "DarkFlame crash report\r\n");
    Write(log, "Module: %s (manual-map base=0x%08llX size=0x%08lX)\r\n",
        g_moduleNameAnsi, static_cast<unsigned long long>(g_imageBase), g_imageSize);
    Write(log, "Reason: %s\r\n", reason ? reason : "unhandled exception");
    Write(log, "Exception: 0x%08lX (%s), address=0x%08llX\r\n",
        code, ExceptionName(code),
        reinterpret_cast<unsigned long long>(exception->ExceptionRecord->ExceptionAddress));
    if(code == EXCEPTION_ACCESS_VIOLATION
        && exception->ExceptionRecord->NumberParameters >= 2)
    {
        const ULONG_PTR operation = exception->ExceptionRecord->ExceptionInformation[0];
        const char* action = operation == 0 ? "read"
            : operation == 1 ? "write" : operation == 8 ? "execute" : "access";
        Write(log, "Access violation: %s address 0x%08llX\r\n", action,
            static_cast<unsigned long long>(
                exception->ExceptionRecord->ExceptionInformation[1]));
    }
    Write(log, "Symbols: %s, image=%ls\r\n", g_symbolsReady ? "PDB loaded" : "unavailable",
        g_imagePath);
    Write(log, "EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\r\n",
        context.Eax, context.Ebx, context.Ecx, context.Edx,
        context.Esi, context.Edi);
    Write(log, "EBP=%08lX ESP=%08lX EIP=%08lX EFlags=%08lX\r\n\r\nStack trace:\r\n",
        context.Ebp, context.Esp, context.Eip, context.EFlags);
    WriteStack(log, context);
    FlushFileBuffers(log);
    CloseHandle(log);
    WriteMiniDump(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

inline LONG WINAPI UnhandledFilter(PEXCEPTION_POINTERS exception)
{
    return WriteReport("unhandled exception", exception);
}

inline LONG CALLBACK VectoredFilter(PEXCEPTION_POINTERS exception)
{
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    const bool immediate = code == EXCEPTION_STACK_OVERFLOW
        || code == 0xC00001A5u || code == 0xC0000409u
        || code == 0xC0000602u;
    if (immediate && IsInMappedImage(exception->ContextRecord->Eip))
        WriteReport("first-chance fatal exception", exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void ReportCurrentAndTerminate(const char* reason, DWORD code)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = reinterpret_cast<void*>(context.Eip);
    EXCEPTION_POINTERS exception{&record, &context};
    WriteReport(reason, &exception);
    TerminateProcess(GetCurrentProcess(), code);
}

inline void __cdecl InvalidParameter(const wchar_t*, const wchar_t*,
    const wchar_t*, unsigned int, uintptr_t)
{
    ReportCurrentAndTerminate("invalid CRT parameter", 0xE0000001u);
}

inline void __cdecl PureCall()
{
    ReportCurrentAndTerminate("pure virtual call", 0xE0000002u);
}

inline void Terminate()
{
    ReportCurrentAndTerminate("std::terminate", 0xE0000003u);
}

inline void __cdecl Signal(int signal)
{
    ReportCurrentAndTerminate("fatal CRT signal", 0xE0000100u + signal);
}

inline bool PatchUnhandledFilter()
{
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    g_filterTarget = kernel32
        ? reinterpret_cast<BYTE*>(GetProcAddress(kernel32, "SetUnhandledExceptionFilter")) : nullptr;
    if (!g_filterTarget)
        return false;

    constexpr BYTE patch[]{0x33, 0xC0, 0xC2, 0x04, 0x00};
    DWORD protection{};
    if (!VirtualProtect(g_filterTarget, sizeof(patch), PAGE_EXECUTE_READWRITE, &protection))
        return false;
    std::memcpy(g_filterBackup, g_filterTarget, sizeof(patch));
    std::memcpy(g_filterTarget, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), g_filterTarget, sizeof(patch));
    DWORD ignored{};
    VirtualProtect(g_filterTarget, sizeof(patch), protection, &ignored);
    g_filterPatched = true;
    return true;
}

inline void UnpatchUnhandledFilter()
{
    if (!g_filterPatched || !g_filterTarget)
        return;
    DWORD protection{};
    if (!VirtualProtect(g_filterTarget, sizeof(g_filterBackup), PAGE_EXECUTE_READWRITE, &protection))
        return;
    std::memcpy(g_filterTarget, g_filterBackup, sizeof(g_filterBackup));
    FlushInstructionCache(GetCurrentProcess(), g_filterTarget, sizeof(g_filterBackup));
    DWORD ignored{};
    VirtualProtect(g_filterTarget, sizeof(g_filterBackup), protection, &ignored);
    g_filterPatched = false;
    g_filterTarget = nullptr;
}

inline bool Install(HMODULE image, std::wstring_view imagePath,
    std::wstring_view moduleName, std::wstring_view artifactDirectory = {})
{
    if (g_installed)
        return true;
    g_imageBase = reinterpret_cast<DWORD64>(image);
    g_imageSize = ReadImageSize(image);
    if (!g_imageBase || !g_imageSize || !Copy(g_imagePath, imagePath)
        || !Copy(g_moduleName, moduleName))
    {
        return false;
    }
    WideCharToMultiByte(CP_UTF8, 0, g_moduleName, -1, g_moduleNameAnsi,
        static_cast<int>(std::size(g_moduleNameAnsi)), nullptr, nullptr);
    BuildArtifactDirectory(artifactDirectory);

    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    g_previousFilter = SetUnhandledExceptionFilter(&UnhandledFilter);
    if (!PatchUnhandledFilter())
    {
        SetUnhandledExceptionFilter(g_previousFilter);
        g_previousFilter = nullptr;
        return false;
    }

    g_vectoredHandler = AddVectoredExceptionHandler(1, &VectoredFilter);
    g_previousInvalidParameter = _set_invalid_parameter_handler(&InvalidParameter);
    g_previousPureCall = _set_purecall_handler(&PureCall);
    g_previousTerminate = std::set_terminate(&Terminate);
    g_previousAbort = std::signal(SIGABRT, &Signal);
    g_symbolsReady = LoadSymbols();
    g_installed = true;
    return true;
}

inline void Shutdown()
{
    if (!g_installed)
        return;
    if (g_vectoredHandler)
        RemoveVectoredExceptionHandler(g_vectoredHandler);
    _set_invalid_parameter_handler(g_previousInvalidParameter);
    _set_purecall_handler(g_previousPureCall);
    std::set_terminate(g_previousTerminate);
    std::signal(SIGABRT, g_previousAbort);
    UnpatchUnhandledFilter();
    SetUnhandledExceptionFilter(g_previousFilter);
    g_installed = false;
}
#else
inline bool Install(HMODULE, std::wstring_view, std::wstring_view) { return false; }
inline void Shutdown() {}
#endif
}
