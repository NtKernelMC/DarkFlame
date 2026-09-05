#include "script_dumper.h"

#include "logger.h"
#include "memory_utils.h"
#include "netc_bitstream.h"
#include "script_dumper_utils.h"
#include "win32_sync.h"
#include "third_party/ByteRevenant/Decompiler.hpp"
#include "third_party/ByteRevenant/Gafnium.hpp"
#include "third_party/zlib/zlib.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <malloc.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
using namespace MemoryUtil;
using namespace ScriptDumperUtil;
using namespace Win32Sync;

constexpr unsigned char ResourceStartPacket = 82;
constexpr unsigned char ResourceStopPacket = 83;
constexpr unsigned char ResourceClientScriptsPacket = 95;
constexpr unsigned int MaxScriptsPerPacket = 4096;
constexpr unsigned int MaxPacketPathBytes = 4096;
constexpr unsigned int MaxCompressedChunkBytes = 16 * 1024 * 1024;
constexpr unsigned int MaxUncompressedScriptBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxIncomingPacketBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxQueuedBytes = 64 * 1024 * 1024;
constexpr std::size_t WorkerStackReserve = 8 * 1024 * 1024;

struct DumpJob
{
    std::filesystem::path relativePath;
    std::vector<std::uint8_t> chunk;
    std::uint32_t expectedChecksum{};
    std::uint64_t sequence{};
};

SRWLOCK g_startLock = SRWLOCK_INIT;
SRWLOCK g_queueLock = SRWLOCK_INIT;
SRWLOCK g_resourceLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_queueCondition = CONDITION_VARIABLE_INIT;
std::atomic_bool g_requested{};
std::atomic_bool g_ready{};
std::atomic_uint g_rejectedPackets{};
std::atomic_uint g_droppedJobs{};
std::atomic_uint64_t g_nextSequence{1};
std::filesystem::path g_dumpRoot;
std::deque<DumpJob> g_jobs;
std::size_t g_queuedBytes{};
std::unordered_map<std::wstring, std::uint64_t> g_latestJobs;
std::unordered_map<unsigned short, std::string> g_resources;
unsigned int g_workerCount{};

std::string ResourceName(unsigned short netId)
{
    SharedLock lock(g_resourceLock);
    const auto found = g_resources.find(netId);
    return found == g_resources.end() ? std::string{} : found->second;
}

bool ResolveRelativePath(unsigned short netId, std::string packetPath,
    std::filesystem::path& output)
{
    std::vector<std::string> components;
    if(!SplitPacketPath(std::move(packetPath), components))
        return false;

    const std::string resource = ResourceName(netId);
    if(components.size() == 1)
    {
        if(!resource.empty()
            && !EqualAsciiInsensitive(components.front(), resource))
        {
            components.insert(components.begin(), resource);
        }
        else if(resource.empty())
        {
            components.insert(components.begin(),
                "resource_" + std::to_string(netId));
        }
    }

    output.clear();
    for(const std::string& component : components)
    {
        const std::wstring wide = Utf8ToWide(component);
        if(wide.empty())
            return false;
        output /= wide;
    }
    return !output.empty() && !output.is_absolute();
}

bool IsLatest(const DumpJob& job)
{
    const std::wstring key = job.relativePath.wstring();
    SharedLock lock(g_queueLock);
    const auto found = g_latestJobs.find(key);
    const bool latest = found != g_latestJobs.end()
        && found->second == job.sequence;
    return latest;
}

void FinishJob(const DumpJob& job) noexcept
{
    try
    {
        const std::wstring key = job.relativePath.wstring();
        ExclusiveLock lock(g_queueLock);
        const auto found = g_latestJobs.find(key);
        if(found != g_latestJobs.end() && found->second == job.sequence)
            g_latestJobs.erase(found);
    }
    catch(...)
    {
    }
}

std::vector<std::uint8_t> Decompress(DumpJob& job)
{
    if(job.chunk.size() < 4)
        throw std::runtime_error("compressed script chunk is shorter than header");
    const std::uint32_t originalLength =
        static_cast<std::uint32_t>(job.chunk[0]) << 24
        | static_cast<std::uint32_t>(job.chunk[1]) << 16
        | static_cast<std::uint32_t>(job.chunk[2]) << 8
        | static_cast<std::uint32_t>(job.chunk[3]);
    if(originalLength > MaxUncompressedScriptBytes)
        throw std::runtime_error("uncompressed script exceeds safety limit");

    for(std::size_t offset = 4; offset < job.chunk.size(); ++offset)
        job.chunk[offset] ^= 0x10;
    std::vector<std::uint8_t> output((std::max)(originalLength, 1u));
    uLongf outputLength = static_cast<uLongf>(output.size());
    const int status = uncompress(output.data(), &outputLength,
        job.chunk.data() + 4, static_cast<uLong>(job.chunk.size() - 4));
    if(status != Z_OK || outputLength != originalLength)
        throw std::runtime_error("zlib script decompression failed");
    output.resize(originalLength);
    const std::uint32_t checksum = static_cast<std::uint32_t>(crc32(0,
        output.data(), static_cast<uInt>(output.size())));
    if(checksum != job.expectedChecksum)
        throw std::runtime_error("script checksum mismatch");
    return output;
}

void LogFailure(const DumpJob& job, std::wstring_view stage,
    std::wstring_view detail)
{
    std::wstring line = L"[scripts-dumper] ";
    line.append(stage);
    line += L" failed for ";
    line += job.relativePath.wstring();
    if(!detail.empty())
    {
        line += L": ";
        line.append(detail);
    }
    Log::Write(line);
}

void ProcessJob(DumpJob& job)
{
    try
    {
        std::vector<std::uint8_t> script = Decompress(job);
        br::DiagnosticSink diagnostics;
        br::GafniumProcessor gafnium;
        br::DeobfuscationResult deobfuscated;
        try
        {
            deobfuscated = gafnium.Process(script, diagnostics);
        }
        catch(const std::exception& error)
        {
            const std::filesystem::path raw = g_dumpRoot
                / RawBytecodePath(job.relativePath);
            if(IsLatest(job))
                AtomicWrite(raw, script, job.sequence);
            LogFailure(job, L"Gafnium", ErrorText(error.what()));
            FinishJob(job);
            return;
        }

        if(!br::GafniumProcessor::LooksLikeLuaBytecode(deobfuscated.data))
        {
            const std::filesystem::path destination = g_dumpRoot
                / job.relativePath;
            if(IsLatest(job)
                && !AtomicWrite(destination, deobfuscated.data, job.sequence))
            {
                LogFailure(job, L"source write", L"Win32 file operation failed");
            }
            FinishJob(job);
            return;
        }

        const std::filesystem::path lua = g_dumpRoot
            / LuaOutputPath(job.relativePath);
        const std::filesystem::path raw = g_dumpRoot
            / RawBytecodePath(job.relativePath);
        if(IsLatest(job) && !AtomicWrite(raw, script, job.sequence))
            LogFailure(job, L"bytecode write", L"Win32 file operation failed");

        br::lua51::Parser parser;
        br::lua51::Chunk chunk = parser.Parse(deobfuscated.data, diagnostics);
        br::lua51::Decompiler decompiler;
        br::lua51::DecompileResult result = decompiler.Decompile(chunk,
            diagnostics);
        if(IsLatest(job) && AtomicWrite(lua, result.lua, job.sequence))
        {
            Log::Write(L"[scripts-dumper] decompiled "
                + job.relativePath.wstring());
        }
        else if(IsLatest(job))
        {
            LogFailure(job, L"decompiled source write",
                L"Win32 file operation failed");
        }
        FinishJob(job);
    }
    catch(const std::exception& error)
    {
        LogFailure(job, L"worker", ErrorText(error.what()));
        FinishJob(job);
    }
    catch(...)
    {
        LogFailure(job, L"worker", L"unknown exception");
        FinishJob(job);
    }
}

bool GuardedProcessJob(DumpJob* job, DWORD* exceptionCode)
{
    __try
    {
        ProcessJob(*job);
        return true;
    }
    __except((*exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER))
    {
        return false;
    }
}

DWORD WINAPI WorkerThread(void*)
{
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    for(;;)
    {
        DumpJob job;
        {
            ExclusiveLock lock(g_queueLock);
            while(g_jobs.empty())
                SleepConditionVariableSRW(&g_queueCondition, &g_queueLock,
                    INFINITE, 0);
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
            g_queuedBytes -= job.chunk.size();
        }
        DWORD exceptionCode{};
        if(GuardedProcessJob(&job, &exceptionCode))
            continue;
        if(exceptionCode == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        wchar_t detail[64]{};
        swprintf_s(detail, L"exception 0x%08lX",
            static_cast<unsigned long>(exceptionCode));
        LogFailure(job, L"worker SEH", detail);
        FinishJob(job);
    }
}

bool QueueJob(std::filesystem::path relativePath,
    std::vector<std::uint8_t> chunk, std::uint32_t expectedChecksum)
{
    const std::uint64_t sequence = g_nextSequence.fetch_add(1,
        std::memory_order_relaxed);
    {
        ExclusiveLock lock(g_queueLock);
        if(chunk.size() <= MaxQueuedBytes
            - (std::min)(g_queuedBytes, MaxQueuedBytes))
        {
            const std::wstring key = relativePath.wstring();
            g_jobs.push_back(DumpJob{std::move(relativePath), std::move(chunk),
                expectedChecksum, sequence});
            try
            {
                g_latestJobs[key] = sequence;
            }
            catch(...)
            {
                g_jobs.pop_back();
                throw;
            }
            g_queuedBytes += g_jobs.back().chunk.size();
            WakeConditionVariable(&g_queueCondition);
            return true;
        }
    }
    if(g_droppedJobs.fetch_add(1, std::memory_order_relaxed) < 4)
        Log::Write(L"[scripts-dumper] worker queue is full; script dropped");
    return false;
}

bool ReadString(PacketReader& stream, std::string& value)
{
    unsigned short length{};
    if(!stream.Read(length) || length > MaxPacketPathBytes)
        return false;
    return stream.Read(value, length);
}

bool RememberResource(PacketReader& stream)
{
    unsigned char nameLength{};
    if(!stream.Read(nameLength) || !nameLength)
        return false;
    std::string name;
    unsigned int startCounter{};
    unsigned short netId{};
    if(!stream.Read(name, nameLength) || !stream.Read(startCounter)
        || !stream.Read(netId))
    {
        return false;
    }
    std::string sanitized;
    if(!SanitizeComponent(name, sanitized))
        return false;
    ExclusiveLock lock(g_resourceLock);
    g_resources[netId] = std::move(sanitized);
    return true;
}

bool ForgetResource(PacketReader& stream)
{
    unsigned short netId{};
    if(!stream.Read(netId))
        return false;
    ExclusiveLock lock(g_resourceLock);
    g_resources.erase(netId);
    return true;
}

bool CaptureScripts(PacketReader& stream)
{
    unsigned short netId{};
    unsigned short scriptCount{};
    if(!stream.Read(netId) || !stream.Read(scriptCount)
        || scriptCount > MaxScriptsPerPacket)
    {
        return false;
    }
    for(unsigned int index{}; index < scriptCount; ++index)
    {
        std::string packetPath;
        unsigned int chunkLength{};
        if(!ReadString(stream, packetPath) || !stream.Read(chunkLength)
            || chunkLength < 4 || chunkLength > MaxCompressedChunkBytes)
        {
            return false;
        }
        std::vector<std::uint8_t> chunk;
        if(!stream.Read(chunk, chunkLength))
        {
            return false;
        }
        unsigned int expectedChecksum{};
        if(!stream.Read(expectedChecksum))
            return false;
        std::filesystem::path relativePath;
        if(!ResolveRelativePath(netId, std::move(packetPath), relativePath))
            return false;
        QueueJob(std::move(relativePath), std::move(chunk), expectedChecksum);
    }
    return true;
}

bool TryPacketView(Netc::BitStream* bitStream, const std::uint8_t** data,
    std::size_t* size)
{
    __try
    {
        const int bits = bitStream->GetNumberOfBitsUsed();
        if(bits <= 0)
            return false;
        const std::size_t bytes = (static_cast<std::size_t>(bits) + 7) >> 3;
        if(bytes > MaxIncomingPacketBytes)
            return false;
        *data = bitStream->GetData();
        *size = bytes;
        return *data && Readable(*data, bytes);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
}

bool ConfigureScriptDumper(bool enabled, std::wstring_view loaderDirectory)
{
    if(!enabled)
    {
        Log::Write(L"[scripts-dumper] disabled");
        return true;
    }
    if(loaderDirectory.empty())
        return false;

    g_dumpRoot = std::filesystem::path(loaderDirectory) / L"DumpedScripts";
    g_requested.store(true, std::memory_order_release);
    return true;
}

bool StartScriptDumperWorkers()
{
    if(!g_requested.load(std::memory_order_acquire))
        return true;

    ExclusiveLock lock(g_startLock);
    if(g_workerCount)
        return true;
    std::error_code error;
    std::filesystem::create_directories(g_dumpRoot, error);
    if(error)
        return false;

    HANDLE thread = CreateThread(nullptr, WorkerStackReserve, &WorkerThread,
        nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if(thread)
    {
        CloseHandle(thread);
        g_workerCount = 1;
    }
    const bool started = g_workerCount != 0;
    g_ready.store(started, std::memory_order_release);
    if(started)
    {
        Log::Write(L"[scripts-dumper] enabled with one isolated worker; output="
            + g_dumpRoot.wstring());
    }
    return started;
}

bool IsScriptDumperEnabled()
{
    return g_requested.load(std::memory_order_acquire);
}

void ObserveIncomingResourcePacket(unsigned char packetId,
    Netc::BitStream* bitStream)
{
    if(!g_ready.load(std::memory_order_acquire) || !bitStream
        || (packetId != ResourceStartPacket && packetId != ResourceStopPacket
            && packetId != ResourceClientScriptsPacket))
    {
        return;
    }

    const std::uint8_t* data{};
    std::size_t size{};
    if(!TryPacketView(bitStream, &data, &size))
    {
        if(g_rejectedPackets.fetch_add(1, std::memory_order_relaxed) < 4)
            Log::Write(L"[scripts-dumper] unreadable resource packet rejected");
        return;
    }

    bool valid{};
    try
    {
        PacketReader reader(std::span<const std::uint8_t>(data, size));
        if(packetId == ResourceStartPacket)
            valid = RememberResource(reader);
        else if(packetId == ResourceStopPacket)
            valid = ForgetResource(reader);
        else
            valid = CaptureScripts(reader);
    }
    catch(...)
    {
        valid = false;
    }
    if(!valid && g_rejectedPackets.fetch_add(1, std::memory_order_relaxed) < 4)
        Log::Write(L"[scripts-dumper] malformed resource packet rejected");
}
