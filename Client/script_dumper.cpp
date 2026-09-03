#include "script_dumper.h"

#include "logger.h"
#include "netc_bitstream.h"
#include "third_party/ByteRevenant/Decompiler.hpp"
#include "third_party/ByteRevenant/Gafnium.hpp"
#include "third_party/zlib/zlib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr unsigned char ResourceStartPacket = 82;
constexpr unsigned char ResourceStopPacket = 83;
constexpr unsigned char ResourceClientScriptsPacket = 95;
constexpr unsigned int MaxScriptsPerPacket = 4096;
constexpr unsigned int MaxPacketPathBytes = 4096;
constexpr unsigned int MaxCompressedChunkBytes = 64 * 1024 * 1024;
constexpr unsigned int MaxUncompressedScriptBytes = 128 * 1024 * 1024;
constexpr std::size_t MaxQueuedBytes = 256 * 1024 * 1024;

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

std::wstring Utf8ToWide(std::string_view text)
{
    if(text.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if(!size)
    {
        codePage = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(codePage, flags, text.data(),
            static_cast<int>(text.size()), nullptr, 0);
    }
    if(!size)
        return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if(!MultiByteToWideChar(codePage, flags, text.data(),
        static_cast<int>(text.size()), output.data(), size))
    {
        return {};
    }
    return output;
}

std::wstring ErrorText(std::string_view text)
{
    std::wstring result = Utf8ToWide(text);
    if(result.empty() && !text.empty())
        result = L"unknown error";
    return result;
}

bool IsReservedDeviceName(std::string value)
{
    const std::size_t dot = value.find('.');
    if(dot != std::string::npos)
        value.resize(dot);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if(value == "CON" || value == "PRN" || value == "AUX" || value == "NUL")
        return true;
    if(value.size() != 4)
        return false;
    return (value.starts_with("COM") || value.starts_with("LPT"))
        && value[3] >= '1' && value[3] <= '9';
}

bool SanitizeComponent(std::string_view input, std::string& output)
{
    if(input.empty() || input == "." || input == ".."
        || input.find('\0') != std::string_view::npos)
    {
        return false;
    }
    output.clear();
    output.reserve(input.size() + 1);
    for(unsigned char ch : input)
    {
        if(ch < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"'
            || ch == '<' || ch == '>' || ch == '|')
        {
            continue;
        }
        output.push_back(static_cast<char>(ch));
    }
    while(!output.empty() && (output.back() == ' ' || output.back() == '.'))
        output.pop_back();
    if(output.empty() || output == "." || output == "..")
        return false;
    if(IsReservedDeviceName(output))
        output.insert(output.begin(), '_');
    return true;
}

bool SplitPacketPath(std::string path, std::vector<std::string>& components)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if(path.empty() || path.front() == '/')
        return false;
    std::size_t offset{};
    while(offset <= path.size())
    {
        const std::size_t separator = path.find('/', offset);
        const std::size_t end = separator == std::string::npos
            ? path.size() : separator;
        std::string component;
        if(!SanitizeComponent(std::string_view(path).substr(offset, end - offset),
            component))
        {
            return false;
        }
        components.push_back(std::move(component));
        if(separator == std::string::npos)
            break;
        offset = separator + 1;
    }
    return !components.empty();
}

bool EqualAsciiInsensitive(std::string_view left, std::string_view right)
{
    if(left.size() != right.size())
        return false;
    for(std::size_t index{}; index < left.size(); ++index)
    {
        if(std::tolower(static_cast<unsigned char>(left[index]))
            != std::tolower(static_cast<unsigned char>(right[index])))
        {
            return false;
        }
    }
    return true;
}

std::string ResourceName(unsigned short netId)
{
    AcquireSRWLockShared(&g_resourceLock);
    const auto found = g_resources.find(netId);
    std::string name = found == g_resources.end() ? std::string{} : found->second;
    ReleaseSRWLockShared(&g_resourceLock);
    return name;
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

bool AtomicWrite(const std::filesystem::path& path,
    std::span<const std::uint8_t> data, std::uint64_t sequence)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if(error)
        return false;

    std::filesystem::path temporary = path;
    temporary += L".darkflame-" + std::to_wstring(sequence) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
        return false;

    bool written = true;
    std::size_t offset{};
    while(offset < data.size())
    {
        const DWORD request = static_cast<DWORD>((std::min)(data.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD count{};
        if(!WriteFile(file, data.data() + offset, request, &count, nullptr)
            || count != request)
        {
            written = false;
            break;
        }
        offset += count;
    }
    written = written && FlushFileBuffers(file);
    CloseHandle(file);
    if(written)
    {
        written = MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if(!written)
        DeleteFileW(temporary.c_str());
    return written;
}

bool AtomicWrite(const std::filesystem::path& path, std::string_view text,
    std::uint64_t sequence)
{
    return AtomicWrite(path,
        std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
        sequence);
}

std::filesystem::path RawBytecodePath(const std::filesystem::path& luaPath)
{
    std::filesystem::path raw = luaPath;
    raw.replace_extension(L".luac");
    return raw;
}

std::filesystem::path LuaOutputPath(const std::filesystem::path& input)
{
    std::filesystem::path output = input;
    output.replace_extension(L".lua");
    return output;
}

bool IsLatest(const DumpJob& job)
{
    AcquireSRWLockShared(&g_queueLock);
    const auto found = g_latestJobs.find(job.relativePath.wstring());
    const bool latest = found != g_latestJobs.end()
        && found->second == job.sequence;
    ReleaseSRWLockShared(&g_queueLock);
    return latest;
}

void FinishJob(const DumpJob& job)
{
    AcquireSRWLockExclusive(&g_queueLock);
    const auto found = g_latestJobs.find(job.relativePath.wstring());
    if(found != g_latestJobs.end() && found->second == job.sequence)
        g_latestJobs.erase(found);
    ReleaseSRWLockExclusive(&g_queueLock);
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

DWORD WINAPI WorkerThread(void*)
{
    for(;;)
    {
        AcquireSRWLockExclusive(&g_queueLock);
        while(g_jobs.empty())
            SleepConditionVariableSRW(&g_queueCondition, &g_queueLock, INFINITE, 0);
        DumpJob job = std::move(g_jobs.front());
        g_jobs.pop_front();
        g_queuedBytes -= job.chunk.size();
        ReleaseSRWLockExclusive(&g_queueLock);
        ProcessJob(job);
    }
}

bool QueueJob(std::filesystem::path relativePath,
    std::vector<std::uint8_t> chunk, std::uint32_t expectedChecksum)
{
    const std::uint64_t sequence = g_nextSequence.fetch_add(1,
        std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_queueLock);
    if(chunk.size() > MaxQueuedBytes - (std::min)(g_queuedBytes, MaxQueuedBytes))
    {
        ReleaseSRWLockExclusive(&g_queueLock);
        if(g_droppedJobs.fetch_add(1, std::memory_order_relaxed) < 4)
            Log::Write(L"[scripts-dumper] worker queue is full; script dropped");
        return false;
    }
    g_queuedBytes += chunk.size();
    g_latestJobs[relativePath.wstring()] = sequence;
    g_jobs.push_back(DumpJob{std::move(relativePath), std::move(chunk),
        expectedChecksum, sequence});
    WakeConditionVariable(&g_queueCondition);
    ReleaseSRWLockExclusive(&g_queueLock);
    return true;
}

bool ReadString(Netc::BitStream& stream, std::string& value)
{
    unsigned short length{};
    if(!stream.Read(length) || length > MaxPacketPathBytes)
        return false;
    value.assign(length, '\0');
    return !length || stream.Read(value.data(), length);
}

bool RememberResource(Netc::BitStream& stream)
{
    unsigned char nameLength{};
    if(!stream.Read(nameLength) || !nameLength)
        return false;
    std::string name(nameLength, '\0');
    unsigned int startCounter{};
    unsigned short netId{};
    if(!stream.Read(name.data(), nameLength) || !stream.Read(startCounter)
        || !stream.Read(netId))
    {
        return false;
    }
    std::string sanitized;
    if(!SanitizeComponent(name, sanitized))
        return false;
    AcquireSRWLockExclusive(&g_resourceLock);
    g_resources[netId] = std::move(sanitized);
    ReleaseSRWLockExclusive(&g_resourceLock);
    return true;
}

bool ForgetResource(Netc::BitStream& stream)
{
    unsigned short netId{};
    if(!stream.Read(netId))
        return false;
    AcquireSRWLockExclusive(&g_resourceLock);
    g_resources.erase(netId);
    ReleaseSRWLockExclusive(&g_resourceLock);
    return true;
}

bool CaptureScripts(Netc::BitStream& stream)
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
        const int unreadBits = stream.GetNumberOfUnreadBits();
        if(unreadBits < 0
            || static_cast<std::uint64_t>(unreadBits) <
                static_cast<std::uint64_t>(chunkLength) * 8)
        {
            return false;
        }
        std::vector<std::uint8_t> chunk(chunkLength);
        if(!stream.Read(reinterpret_cast<char*>(chunk.data()),
            static_cast<int>(chunk.size())))
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

    AcquireSRWLockExclusive(&g_startLock);
    if(g_workerCount)
    {
        ReleaseSRWLockExclusive(&g_startLock);
        return true;
    }
    std::error_code error;
    std::filesystem::create_directories(g_dumpRoot, error);
    if(error)
    {
        ReleaseSRWLockExclusive(&g_startLock);
        return false;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const unsigned int processors = systemInfo.dwNumberOfProcessors;
    const unsigned int requested = (std::clamp)(processors, 2u, 4u);
    for(unsigned int index{}; index < requested; ++index)
    {
        HANDLE thread = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
        if(!thread)
            break;
        CloseHandle(thread);
        ++g_workerCount;
    }
    const bool started = g_workerCount != 0;
    g_ready.store(started, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_startLock);
    if(started)
    {
        Log::Write(L"[scripts-dumper] enabled with "
            + std::to_wstring(g_workerCount) + L" workers; output="
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

    bool valid{};
    int originalOffset{};
    try
    {
        originalOffset = bitStream->GetReadOffsetAsBits();
        bitStream->SetReadOffsetAsBits(0);
        if(packetId == ResourceStartPacket)
            valid = RememberResource(*bitStream);
        else if(packetId == ResourceStopPacket)
            valid = ForgetResource(*bitStream);
        else
            valid = CaptureScripts(*bitStream);
        bitStream->SetReadOffsetAsBits(originalOffset);
    }
    catch(...)
    {
        try
        {
            bitStream->SetReadOffsetAsBits(originalOffset);
        }
        catch(...)
        {
        }
        valid = false;
    }
    if(!valid && g_rejectedPackets.fetch_add(1, std::memory_order_relaxed) < 4)
        Log::Write(L"[scripts-dumper] malformed resource packet rejected");
}
