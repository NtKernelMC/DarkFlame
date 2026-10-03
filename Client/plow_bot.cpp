#include "plow_bot.h"
#include "ui_widgets.h"
#include "embedded_lua_runtime.h"
#include "../Shared/runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace
{
std::mutex g_mutex;
std::map<std::string, std::string> g_state;
std::deque<std::string> g_commands;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::string g_path;
ULONGLONG g_heartbeat{};
unsigned long long g_generation{};
std::string g_lease;
std::mutex g_marksMutex;
std::wstring g_marksPath;
bool g_marksBackedUp{};

struct LoaderInfo
{
    std::string version;
    std::size_t bytes{};
    PlowLoad kind{PlowLoad::Waiting};
    std::string text{"Жду мост Lua DarkFlame"};
};
std::mutex g_loaderMutex;
LoaderInfo g_loader;
std::atomic_bool g_reloadRequest{};
std::atomic_bool g_autoReload{true};
std::atomic_bool g_autoRecord{true};
std::wstring g_settingsPath;

struct LogBatch
{
    std::string text;
    bool flush{};
};

struct LogWriter
{
    std::mutex mutex;
    std::deque<LogBatch> queue;
    HANDLE wake{};
    HANDLE thread{};
    std::string error;
    size_t pendingBytes{};
    unsigned long long bytes{}, submitted{}, completed{}, flushed{};
};

// Process lifetime: no thread join or C++ destructor work under the DLL loader lock.
LogWriter& Writer()
{
    static auto* writer = new LogWriter;
    return *writer;
}

DWORD WINAPI WriteLogs(void*)
{
    auto& writer = Writer();
    ULONGLONG lastFlush = GetTickCount64();
    bool dirty = false;
    unsigned long long completed{};
    for(;;)
    {
        WaitForSingleObject(writer.wake, 250);
        std::deque<LogBatch> batches;
        {
            std::scoped_lock lock(writer.mutex);
            batches.swap(writer.queue);
        }
        std::string text;
        bool force = false;
        for(const auto& batch : batches)
        {
            text += batch.text;
            force |= batch.flush;
        }
        DWORD written{};
        std::string error;
        if(!text.empty())
        {
            if(!WriteFile(g_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
                error = "WriteFile: " + std::to_string(GetLastError());
            else if(written != text.size()) error = "WriteFile: incomplete batch";
            dirty = true;
        }
        bool flushed = false;
        if(error.empty() && (force || (dirty && GetTickCount64() - lastFlush >= 1000)))
        {
            if(!FlushFileBuffers(g_file)) error = "FlushFileBuffers: " + std::to_string(GetLastError());
            else
            {
                dirty = false;
                flushed = true;
                lastFlush = GetTickCount64();
            }
        }
        completed += batches.size();
        {
            std::scoped_lock lock(writer.mutex);
            writer.bytes += written;
            writer.pendingBytes -= text.size();
            writer.completed = completed;
            if(flushed) writer.flushed = completed;
            if(!error.empty())
            {
                writer.error = std::move(error);
                writer.queue.clear();
                writer.pendingBytes = 0;
                return 0;
            }
        }
    }
}

std::string QueueLog(std::string_view text, bool flush)
{
    auto& writer = Writer();
    std::scoped_lock lock(writer.mutex);
    if(!writer.error.empty()) return writer.error;
    if(g_file == INVALID_HANDLE_VALUE) return "Log file is not initialized";
    if(text.empty() && (!flush || writer.submitted == writer.flushed)) return {};
    if(text.size() > 4 * 1024 * 1024 - writer.pendingBytes
        || writer.submitted - writer.completed >= 2048)
    {
        writer.error = "Log queue full (4 MB): recording stopped";
        return writer.error;
    }
    if(!writer.thread)
    {
        writer.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if(!writer.wake) writer.error = "CreateEvent: " + std::to_string(GetLastError());
        else
        {
            writer.thread = CreateThread(nullptr, 0, &WriteLogs, nullptr, 0, nullptr);
            if(!writer.thread) writer.error = "CreateThread: " + std::to_string(GetLastError());
        }
        if(!writer.error.empty()) return writer.error;
    }
    writer.queue.push_back({std::string(text), flush});
    writer.pendingBytes += text.size();
    ++writer.submitted;
    SetEvent(writer.wake);
    return {};
}

bool CurrentCollector(void* lua, int index)
{
    size_t size{};
    const char* lease = DarkFlameLuaToLString(lua, index, &size);
    return lease && !g_lease.empty() && std::string_view(lease, size) == g_lease;
}

int PlowLog(void* lua)
{
    size_t size{};
    const char* text = DarkFlameLuaToLString(lua, 1, &size);
    std::scoped_lock lock(g_mutex);
    if(!CurrentCollector(lua, 3))
    {
        DarkFlameLuaPushBoolean(lua, false);
        DarkFlameLuaPushString(lua, "collector_replaced");
        return 2;
    }
    const bool valid = text && size <= 65536;
    const std::string error = valid ? QueueLog(std::string_view(text, size),
        DarkFlameLuaToBoolean(lua, 2) != 0) : "Invalid log batch";
    DarkFlameLuaPushBoolean(lua, error.empty());
    DarkFlameLuaPushString(lua, error.c_str());
    return 2;
}

int PlowUpdate(void* lua)
{
    size_t keySize{}, valueSize{};
    const char* key = DarkFlameLuaToLString(lua, 1, &keySize);
    const char* value = DarkFlameLuaToLString(lua, 2, &valueSize);
    if(!key || !value || keySize > 64 || valueSize > 4096) return 0;
    std::scoped_lock lock(g_mutex);
    const std::string name(key, keySize);
    if(name == "attach")
    {
        g_lease = std::to_string(++g_generation);
        g_commands.clear();
        g_state.clear();
        g_heartbeat = 0;
        DarkFlameLuaPushString(lua, g_lease.c_str());
        return 1;
    }
    if(!CurrentCollector(lua, 3))
    {
        DarkFlameLuaPushBoolean(lua, false);
        return 1;
    }
    if(g_state.size() >= 48 && !g_state.contains(name)) return 0;
    if(name == "loaded")
    {
        g_commands.clear();
        g_state.clear();
    }
    if(name == "heartbeat") g_heartbeat = GetTickCount64();
    g_state[name] = std::string(value, valueSize);
    DarkFlameLuaPushBoolean(lua, true);
    return 1;
}

int PlowTakeCommand(void* lua)
{
    std::scoped_lock lock(g_mutex);
    if(!CurrentCollector(lua, 1)) return 0;
    if(GetTickCount64() - g_heartbeat > 2500) g_commands.clear();
    if(g_commands.empty()) return 0;
    DarkFlameLuaPushString(lua, g_commands.front().c_str());
    g_commands.pop_front();
    return 1;
}

bool ReadMarksFile(const std::wstring& path, std::string& text, std::string& error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        error = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
            ? "missing" : "CreateFile: " + std::to_string(code);
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 256 * 1024;
    if(!ok) error = "invalid size";
    else
    {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read{};
        ok = text.empty() || (ReadFile(file, text.data(), static_cast<DWORD>(text.size()),
            &read, nullptr) && read == text.size());
        if(!ok) error = "ReadFile: " + std::to_string(GetLastError());
    }
    CloseHandle(file);
    if(ok && text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }
    if(ok && text.find('\0') != std::string::npos)
    {
        ok = false;
        error = "zero byte";
    }
    return ok;
}

bool WriteMarksFile(const std::wstring& path, std::string_view text, std::string& error)
{
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
    {
        error = "CreateFile: " + std::to_string(GetLastError());
        return false;
    }
    DWORD written{};
    bool ok = text.empty() || (WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
        &written, nullptr) && written == text.size());
    if(!ok) error = "WriteFile: " + std::to_string(GetLastError());
    CloseHandle(file);
    if(ok && !MoveFileExW(temp.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        ok = false;
        error = "MoveFileEx: " + std::to_string(GetLastError());
    }
    if(!ok) DeleteFileW(temp.c_str());
    return ok;
}

void LoadSettings()
{
    std::wstring path;
    {
        std::scoped_lock lock(g_marksMutex);
        path = g_settingsPath;
    }
    std::string text, error;
    if(!path.empty() && ReadMarksFile(path, text, error))
        g_autoRecord.store(text.find("AUTORECORD=0") == std::string::npos);
}

void SaveSettings()
{
    std::wstring path;
    {
        std::scoped_lock lock(g_marksMutex);
        path = g_settingsPath;
    }
    std::string error;
    if(!path.empty())
        WriteMarksFile(path, g_autoRecord.load() ? "AUTORECORD=1\n" : "AUTORECORD=0\n", error);
}

int PlowMarks(void* lua)
{
    size_t actionSize{}, textSize{};
    const char* action = DarkFlameLuaToLString(lua, 1, &actionSize);
    const char* text = DarkFlameLuaToLString(lua, 2, &textSize);
    std::wstring path;
    {
        std::scoped_lock lock(g_mutex);
        if(!CurrentCollector(lua, 3))
        {
            DarkFlameLuaPushNil(lua);
            DarkFlameLuaPushString(lua, "collector_replaced");
            return 2;
        }
    }
    std::scoped_lock marksLock(g_marksMutex);
    path = g_marksPath;
    if(!action || path.empty())
    {
        DarkFlameLuaPushNil(lua);
        DarkFlameLuaPushString(lua, path.empty() ? "no_path" : "bad_action");
        return 2;
    }
    const std::string_view name(action, actionSize);
    std::string error;
    if(name == "load")
    {
        std::string content;
        if(!ReadMarksFile(path, content, error))
        {
            DarkFlameLuaPushNil(lua);
            DarkFlameLuaPushString(lua, error.c_str());
            return 2;
        }
        DarkFlameLuaPushString(lua, content.c_str());
        return 1;
    }
    if(name == "save" && text && textSize <= 256 * 1024)
    {
        if(!g_marksBackedUp)
        {
            g_marksBackedUp = true;
            std::wstring backup = path;
            const auto dot = backup.find_last_of(L'.');
            backup = (dot == std::wstring::npos ? backup : backup.substr(0, dot)) + L".bak";
            if(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(path.c_str(), backup.c_str(), FALSE);
        }
        const bool ok = WriteMarksFile(path, std::string_view(text, textSize), error);
        DarkFlameLuaPushBoolean(lua, ok);
        DarkFlameLuaPushString(lua, error.c_str());
        return 2;
    }
    DarkFlameLuaPushNil(lua);
    DarkFlameLuaPushString(lua, "bad_action");
    return 2;
}

void Queue(std::string command)
{
    std::scoped_lock lock(g_mutex);
    if(g_commands.size() < 16) g_commands.push_back(std::move(command));
}
}

void PlowQueueAdminCaption(std::string caption)
{
    if(caption.empty()) return;
    std::scoped_lock lock(g_mutex);
    if(GetTickCount64() - g_heartbeat > 2500) return;
    if(g_commands.size() < 16) g_commands.push_back("admin:" + std::move(caption));
}

void PlowSetScript(std::string version, std::size_t bytes)
{
    std::scoped_lock lock(g_loaderMutex);
    g_loader.version = std::move(version);
    g_loader.bytes = bytes;
}

void PlowSetLoad(PlowLoad kind, std::string text)
{
    std::scoped_lock lock(g_loaderMutex);
    g_loader.kind = kind;
    g_loader.text = std::move(text);
}

bool PlowTakeReload()
{
    return g_reloadRequest.exchange(false);
}

bool PlowAutoReload()
{
    return g_autoReload.load();
}

bool PlowBotBusy()
{
    std::scoped_lock lock(g_mutex);
    if(GetTickCount64() - g_heartbeat > 2500) return false;
    const auto flag = [](const char* key)
    {
        const auto found = g_state.find(key);
        return found != g_state.end() && found->second == "1";
    };
    return flag("loaded") && (flag("bot") || flag("autonomy") || flag("dtp_hold"));
}

std::string PlowScriptVersion(std::string_view code)
{
    constexpr std::string_view key = "VERSION = \"";
    const auto at = code.substr(0, std::min<std::size_t>(code.size(), 8192)).find(key);
    if(at == std::string_view::npos) return "?";
    const auto from = at + key.size();
    const auto end = code.find('"', from);
    if(end == std::string_view::npos || end - from > 24) return "?";
    return std::string(code.substr(from, end - from));
}

void InitializePlowBot()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        std::scoped_lock lock(g_mutex);
        std::wstring path = RuntimeLog::Path();
        const auto slash = path.find_last_of(L"\\/");
        path.resize(slash == std::wstring::npos ? 0 : slash + 1);
        {
            std::scoped_lock marksLock(g_marksMutex);
            g_marksPath = path + L"PlowMarks.txt";
            g_settingsPath = path + L"PlowBot.cfg";
        }
        LoadSettings();
        path += L"PlowBot.log";
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(),
            static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
        g_path.resize(bytes);
        WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
            g_path.data(), bytes, nullptr, nullptr);
        g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(g_file == INVALID_HANDLE_VALUE)
        {
            Writer().error = "CreateFile: " + std::to_string(GetLastError());
            return;
        }
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char header[256]{};
        sprintf_s(header, "{\"type\":\"game_start\",\"schema\":1,\"bot\":\"plow\",\"pid\":%lu,"
            "\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\"}\n",
            GetCurrentProcessId(), now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        DWORD written{};
        const DWORD length = static_cast<DWORD>(strlen(header));
        if(!WriteFile(g_file, header, length, &written, nullptr) || written != length)
            Writer().error = "WriteFile (header): " + std::to_string(GetLastError());
        else if(!FlushFileBuffers(g_file))
            Writer().error = "FlushFileBuffers (header): " + std::to_string(GetLastError());
        Writer().bytes = written;
    });
}

void RegisterPlowBotLua(void* lua)
{
    DarkFlameLuaRegister(lua, "dfPlowLog", &PlowLog);
    DarkFlameLuaRegister(lua, "dfPlowUpdate", &PlowUpdate);
    DarkFlameLuaRegister(lua, "dfPlowTakeCommand", &PlowTakeCommand);
    DarkFlameLuaRegister(lua, "dfPlowMarks", &PlowMarks);
}

void DrawPlowBot(ImVec2 position, ImVec2 size, float scale)
{
    std::map<std::string, std::string> state;
    std::string error, path;
    ULONGLONG age{};
    {
        std::scoped_lock lock(g_mutex);
        state = g_state;
        path = g_path;
        age = GetTickCount64() - g_heartbeat;
    }
    {
        auto& writer = Writer();
        std::scoped_lock lock(writer.mutex);
        error = writer.error;
    }
    LoaderInfo loader;
    {
        std::scoped_lock lock(g_loaderMutex);
        loader = g_loader;
    }
    const bool online = state["loaded"] == "1" && age < 2500;
    const bool ready = online && state["resource_ready"] == "1";
    const bool bot = online && state["bot"] == "1";
    const bool spray = online && state["spray"] == "1";
    const bool crashHold = online && !bot && state["dtp_hold"] == "1";

    static bool editorPage = false;
    static ULONGLONG editorTick{};
    if(online && (state["editor"] == "1") != editorPage && GetTickCount64() - editorTick > 700)
    {
        editorTick = GetTickCount64();
        Queue(editorPage ? "editor:1" : "editor:0");
    }
    static ULONGLONG recordTick{};
    const bool autoRecord = g_autoRecord.load();
    if(online && !state["autorecord"].empty() && (state["autorecord"] == "1") != autoRecord
        && GetTickCount64() - recordTick > 700)
    {
        recordTick = GetTickCount64();
        Queue(autoRecord ? "autorecord:1" : "autorecord:0");
    }

    std::array<std::string, 16> values;
    values.fill("--");
    const auto& dashboard = state["dashboard"];
    size_t offset = 0;
    for(auto& value : values)
    {
        if(!online || offset >= dashboard.size()) break;
        const auto end = dashboard.find('|', offset);
        value = dashboard.substr(offset, end == std::string::npos ? end : end - offset);
        if(end == std::string::npos) break;
        offset = end + 1;
    }

    const ImU32 white = IM_COL32(235, 232, 247, 255);
    const ImU32 muted = IM_COL32(157, 148, 179, 255);
    const ImU32 violet = IM_COL32(190, 100, 255, 255);
    const ImU32 mint = IM_COL32(106, 234, 194, 255);
    const ImU32 amber = IM_COL32(255, 198, 109, 255);
    const ImU32 red = IM_COL32(255, 104, 141, 255);
    const ImU32 statusColor = !online ? muted : bot ? mint : violet;
    static int page = 0;
    editorPage = page == 2;

    ImGui::SetCursorScreenPos(position);
    ImGui::PushFont(ImGui::GetFont(), 19 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * scale, 6 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12 * scale, 8 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12 * scale);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 15, 29, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(61, 42, 82, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(51, 33, 73, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(84, 43, 116, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(108, 46, 155, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(12, 10, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(49, 31, 70, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(70, 37, 100, 255));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, violet);
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, muted);
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(61, 42, 82, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(76, 36, 107, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(94, 43, 128, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(128, 56, 180, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(12, 10, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(76, 51, 99, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(112, 63, 151, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(134, 67, 176, 255));
    ImGui::BeginChild("##plow_bot", size, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const auto colored = [](ImU32 color, const std::string& value)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", value.c_str());
        ImGui::PopStyleColor();
    };
    const auto section = [&](const char* label)
    {
        colored(muted, label);
        ImGui::Separator();
    };
    const auto action = [&](const char* label, const char* command, bool enabled = true,
        float width = -1.0f)
    {
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(label, ImVec2(width, 38 * scale));
        if(pressed) Queue(command);
        ImGui::EndDisabled();
        return pressed;
    };
    const auto line = [&](const std::string& value, ImU32 color)
    {
        // Keep long live status strings on one line; the full value is available on hover.
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float available = ImGui::GetContentRegionAvail().x;
        const bool clipped = ImGui::CalcTextSize(value.c_str()).x > available;
        const float reserve = clipped ? ImGui::CalcTextSize("...").x : 0.0f;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(start, ImVec2(start.x + std::max(0.0f, available - reserve),
            start.y + ImGui::GetTextLineHeight()), true);
        draw->AddText(start, color, value.c_str());
        draw->PopClipRect();
        if(clipped) draw->AddText(ImVec2(start.x + available - reserve, start.y), color, "...");
        ImGui::Dummy(ImVec2(available, ImGui::GetTextLineHeight()));
        if(clipped && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", value.c_str());
    };

    // This header and navigation stay outside the independently scrolling panes.
    ImGui::BeginChild("##plow_header", ImVec2(0, 100 * scale), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if(ImGui::BeginTable("##plow_heading", 3, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("autorecord", ImGuiTableColumnFlags_WidthFixed, 160 * scale);
        ImGui::TableSetupColumn("record", ImGuiTableColumnFlags_WidthFixed, 268 * scale);
        ImGui::TableNextColumn();
        ImGui::PushFont(ImGui::GetFont(), 27 * scale);
        colored(white, "ПОЛИВАЛКА");
        ImGui::PopFont();
        ImGui::SameLine(0, 24 * scale);
        colored(crashHold ? red : statusColor, !online ? "OFFLINE" : bot ? "BOT ACTIVE"
            : crashHold ? "CRASH HOLD" : "STANDBY");
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (38 * scale - ImGui::GetFrameHeight()) / 2);
        bool autoRecordBox = autoRecord;
        if(ImGui::Checkbox("Автозапись###plow_autorecord", &autoRecordBox))
        {
            g_autoRecord.store(autoRecordBox);
            SaveSettings();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Включать запись телеметрии при запуске бота.\n"
                "Без галочки запись — только кнопкой справа.");
        ImGui::TableNextColumn();
        const bool recording = online && state["recording"] == "1";
        ImGui::PushStyleColor(ImGuiCol_Button, recording
            ? IM_COL32(126, 34, 64, 255) : IM_COL32(51, 33, 73, 255));
        action(recording ? "Остановить запись###plow_record" : "Записать телеметрию###plow_record",
            recording ? "record_stop" : "record_start", online);
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    line((state["route"].empty() ? "Маршрут не выбран" : state["route"]) + std::string("  /  ")
        + (!online ? "Скрипт: " + loader.text
            : state["status"].empty() ? "Готов к запуску" : state["status"]), muted);
    ImGui::EndChild();

    const char* pages[]{"Обзор", "Светофоры и метки", "Редактор меток"};
    const float navWidth = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    for(int i = 0; i < 3; ++i)
    {
        if(i) ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, page == i
            ? IM_COL32(108, 46, 155, 255) : IM_COL32(31, 23, 45, 255));
        if(ImGui::Button(pages[i], ImVec2(navWidth, 38 * scale))) page = i;
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    editorPage = page == 2;

    const float bodyHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y);
    const float sidebarWidth = (size.x / scale < 1150 ? 300 : 340) * scale;
    if(!editorPage)
    {
        ImGui::BeginChild("##plow_controls", ImVec2(sidebarWidth, bodyHeight), ImGuiChildFlags_Borders);
        section("УПРАВЛЕНИЕ");
        colored(crashHold ? red : statusColor, crashHold ? "ДТП: стою на аварийке"
            : bot ? "Бот работает" : ready ? "Готов к запуску" : "Ожидание подключения");
        ImGui::PushStyleColor(ImGuiCol_Button, bot
            ? IM_COL32(126, 34, 64, 255) : IM_COL32(72, 38, 112, 255));
        action(bot ? "Остановить бота###plow_toggle" : crashHold ? "Продолжить рейс###plow_toggle"
            : "Запустить бота###plow_toggle", bot ? "bot_stop" : "bot_start", ready || bot);
        ImGui::PopStyleColor();
        ImGui::BeginDisabled(!online);
        bool autonomy = state["autonomy"] == "1";
        if(ImGui::Checkbox("Автономность###plow_autonomy", &autonomy))
            Queue(autonomy ? "autonomy:1" : "autonomy:0");
        bool debug = state["debug"] == "1";
        if(ImGui::Checkbox("Отладка###plow_debug", &debug)) Queue(debug ? "debug:1" : "debug:0");
        bool signals = state["signals"] == "1";
        if(ImGui::Checkbox("Поворотники###plow_signals", &signals))
            Queue(signals ? "signals:1" : "signals:0");
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Бот сам включает поворотник перед поворотом и гасит после");
        bool dtpStop = state["dtp_stop"] == "1";
        if(ImGui::Checkbox("Останавливаться при ДТП###plow_dtp", &dtpStop))
            Queue(dtpStop ? "dtp_stop:1" : "dtp_stop:0");
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Удар: сирена, полная остановка и аварийка.\n"
                "«Продолжить рейс» — бот поедет дальше с этого места.");
        colored(muted, "Лимит скорости по спидометру");
        static int limit = 50;
        static std::string lastLimit;
        if(lastLimit != state["speed_limit"] && !state["speed_limit"].empty())
        {
            lastLimit = state["speed_limit"];
            limit = std::atoi(lastLimit.c_str());
        }
        ImGui::SetNextItemWidth(-1);
        if(DarkFlameSliderInt("##plow_limit", &limit, 20, 90, "%d км/ч", scale))
        {
            Queue("speed_limit:" + std::to_string(limit));
            lastLimit = std::to_string(limit);
        }
        ImGui::EndDisabled();
        colored(spray ? mint : muted, spray ? "Установка включена" : "Установка выключена");
        ImGui::Spacing();
        section("СКРИПТ БОТА");
        const ImU32 loadColor = loader.kind == PlowLoad::Loaded ? mint
            : loader.kind == PlowLoad::Error ? red : loader.kind == PlowLoad::Waiting ? muted : amber;
        colored(white, loader.version.empty() ? "PlowBot.lua не прочитан"
            : "PlowBot " + loader.version + "  /  " + std::to_string((loader.bytes + 512) / 1024) + " КБ");
        colored(loadColor, loader.text);
        bool autoReload = g_autoReload.load();
        if(ImGui::Checkbox("Автообновление###plow_auto_reload", &autoReload)) g_autoReload.store(autoReload);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Подхватывать новую версию скрипта автоматически");
        if(ImGui::Button(loader.kind == PlowLoad::Unloaded ? "Загрузить скрипт###plow_reload"
            : "Перезагрузить скрипт###plow_reload", ImVec2(-1, 38 * scale))) g_reloadRequest.store(true);
        ImGui::EndChild();
        ImGui::SameLine();
    }

    ImGui::BeginChild("##plow_workspace", ImVec2(0, bodyHeight), ImGuiChildFlags_Borders,
        editorPage ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    if(page == 0)
    {
        section("ТЕЛЕМЕТРИЯ");
        static const char* labels[12]{"СКОРОСТЬ", "ТОЧКА", "ВОДА", "СВЕТОФОР",
            "ДО ТОЧКИ", "ЗАПАС ВОДЫ", "РУЛЬ", "ГАЗ", "ПОМЕХА", "ВОДА, Л", "ЗАСТРЯЛ", "РЕЙСОВ"};
        const int columns = ImGui::GetContentRegionAvail().x / scale < 650 ? 2 : 3;
        if(ImGui::BeginTable("##plow_metrics", columns, ImGuiTableFlags_SizingStretchSame))
        {
            for(int i = 0; i < 12; ++i)
            {
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 10, 23, 255));
                ImGui::BeginChild("metric", ImVec2(0, 78 * scale), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PushFont(ImGui::GetFont(), 13 * scale);
                line(labels[i], muted);
                ImGui::PopFont();
                ImU32 color = values[i] == "--" ? muted : i == 0 ? mint : i == 2 ? violet : white;
                if(i == 3 && values[i].rfind("КРАС", 0) == 0) color = red;
                if(i == 3 && values[i].rfind("ЗЕЛ", 0) == 0) color = mint;
                if(i == 5 && !values[i].empty() && values[i][0] == '-') color = red;
                ImGui::PushFont(ImGui::GetFont(), 27 * scale);
                line(values[i], color);
                ImGui::PopFont();
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if(ImGui::CollapsingHeader("Отчёт бота", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if(!error.empty()) colored(red, error);
            colored(white, state["report"].empty() ? "Бот ещё не присылал отчёт." : state["report"]);
            if(!path.empty()) colored(muted, path);
        }
    }
    else if(page == 1)
    {
        ImGui::BeginChild("##plow_lights_card", ImVec2(0, 0),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        section("СВЕТОФОРЫ");
        const bool lightsWork = state["traffic_working"] == "1";
        colored(lightsWork ? mint : amber, std::string(lightsWork ? "Светофоры работают"
            : "Светофоры не работают — бот их не ждёт") + "  /  состояние: "
            + (state["traffic_state"].empty() ? "--" : state["traffic_state"]));
        ImGui::BeginDisabled(!online);
        bool ignoreTraffic = state["ignore_traffic"] == "1";
        if(ImGui::Checkbox("Игнорировать светофоры###plow_ignore_lights", &ignoreTraffic))
            Queue(ignoreTraffic ? "ignore_traffic:1" : "ignore_traffic:0");
        ImGui::EndDisabled();
        if(ImGui::BeginTable("##plow_light_actions", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn(); action("Тест светофора###plow_light_test", "test_traffic", online);
            ImGui::TableNextColumn(); action("Сохранить на зелёный###plow_light_save", "save_traffic", online);
            ImGui::TableNextColumn(); action("Забыть ближайший###plow_light_forget", "forget_traffic", online);
            ImGui::TableNextColumn(); action("Выгрузить в лог###plow_light_dump", "dump_traffic", online);
            ImGui::EndTable();
        }
        colored(muted, state["traffic_info"].empty() ? "Светофоры не обучены" : state["traffic_info"]);
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::BeginChild("##plow_marks_card", ImVec2(0, 0),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        section("СОЗДАТЬ МЕТКУ");
        if(ImGui::BeginTable("##plow_mark_sizes", 3, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(28, 78, 52, 255));
            ImGui::TableNextColumn(); action("Малая###plow_wp_small", "save_waypoint:small", online);
            ImGui::TableNextColumn(); action("Средняя###plow_wp_mid", "save_waypoint:mid", online);
            ImGui::TableNextColumn(); action("Большая###plow_wp_big", "save_waypoint:big", online);
            ImGui::PopStyleColor();
            ImGui::EndTable();
        }
        const std::string lastWp = state["waypoint_last"].empty() ? "--" : state["waypoint_last"];
        colored(muted, "Размер метки #" + lastWp + "  /  проезд "
            + (state["waypoint_cursor"].empty() ? "1" : state["waypoint_cursor"])
            + " из " + (state["waypoints"].empty() ? "0" : state["waypoints"]));
        static int wpSize = 14;
        static std::string lastWpSize;
        if(lastWpSize != state["waypoint_size"] && !state["waypoint_size"].empty())
        {
            lastWpSize = state["waypoint_size"];
            wpSize = std::atoi(lastWpSize.c_str());
        }
        ImGui::BeginDisabled(!online || lastWp == "--");
        ImGui::SetNextItemWidth(-1);
        if(DarkFlameSliderInt("##plow_wp_size", &wpSize, 2, 50, "%d м", scale))
        {
            Queue("waypoint_size:" + std::to_string(wpSize));
            lastWpSize = std::to_string(wpSize);
        }
        ImGui::EndDisabled();
        if(ImGui::BeginTable("##plow_mark_actions", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            action("Убрать ближайшую метку###plow_wp_forget", "forget_waypoint", online);
            ImGui::TableNextColumn();
            action("Выгрузить метки в лог###plow_wp_dump", "dump_waypoints", online);
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    else
    {
        const auto split = [](const std::string& value, char separator)
        {
            std::vector<std::string> parts;
            size_t from = 0;
            while(from <= value.size())
            {
                const auto next = value.find(separator, from);
                parts.push_back(value.substr(from, next == std::string::npos ? std::string::npos : next - from));
                if(next == std::string::npos) break;
                from = next + 1;
            }
            return parts;
        };
        // Выбранное: «номер|x|y|курс|зона|маршрут|расстояние|отмен|подпись»; у точки города номер 0,
        // а подпись — её роль (gate — ворота депо, finish — финиш).
        auto edit = online ? split(state["wp_edit"], '|') : std::vector<std::string>{};
        edit.resize(9);
        const int selected = edit[0].empty() ? 0 : std::atoi(edit[0].c_str());
        const int undo = edit[7].empty() ? 0 : std::atoi(edit[7].c_str());
        const std::string cityPoint = online && selected == 0 && (edit[8] == "gate" || edit[8] == "finish")
            ? edit[8] : std::string();
        const bool has = online && (selected > 0 || !cityPoint.empty());
        const auto distanceText = [](const std::string& value)
        {
            return value.empty() || value[0] == '-' ? std::string("--") : value + " м";
        };
        const auto pointName = [](const std::string& role)
        {
            return role == "gate" ? std::string("Ворота депо") : std::string("Финиш");
        };
        const ImU32 pink = IM_COL32(255, 90, 210, 255);
        const ImU32 sky = IM_COL32(110, 200, 255, 255);
        // Город эдитора и его маршруты: «номер:название:меток».
        const auto cities = online ? split(state["ed_cities"], '|') : std::vector<std::string>{};
        const int cityIndex = std::atoi(state["ed_city"].c_str());
        const std::string cityName = cityIndex >= 1 && cityIndex <= static_cast<int>(cities.size())
            ? cities[cityIndex - 1] : std::string();
        struct RouteRow { std::string id, name, count; };
        std::vector<RouteRow> routes;
        if(online)
        {
            for(const auto& item : split(state["ed_routes"], ';'))
            {
                const auto row = split(item, ':');
                if(row.size() >= 3 && !row[0].empty()) routes.push_back({row[0], row[1], row[2]});
            }
        }
        const std::string routePick = state["ed_route"].empty() ? "0" : state["ed_route"];
        const auto routeTitle = [](const RouteRow& route)
        {
            return "Маршрут " + route.id + (route.name.empty() ? std::string() : "  " + route.name)
                + "  (меток " + route.count + ")";
        };
        const auto cityButtons = [&](const char* id, float height)
        {
            if(!ImGui::BeginTable(("##" + std::string(id)).c_str(), std::max(1, static_cast<int>(cities.size())),
                ImGuiTableFlags_SizingStretchSame))
                return;
            for(size_t i = 0; i < cities.size(); ++i)
            {
                ImGui::TableNextColumn();
                const bool active = static_cast<int>(i) + 1 == cityIndex;
                ImGui::PushStyleColor(ImGuiCol_Button, active ? IM_COL32(108, 46, 155, 255) : IM_COL32(51, 33, 73, 255));
                const std::string label = cities[i] + "###" + id + std::to_string(i);
                if(ImGui::Button(label.c_str(), ImVec2(-1, height * scale))) Queue("ed_city:" + std::to_string(i + 1));
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        };
        const float editorHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y);
        const float listWidth = std::min(380 * scale, ImGui::GetContentRegionAvail().x * 0.38f);
        ImGui::BeginChild("##plow_mark_list", ImVec2(listWidth, editorHeight), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        section("ГОРОД");
        ImGui::BeginDisabled(!online);
        cityButtons("plow_ed_city", 34);
        std::string preview = "Все маршруты города";
        for(const auto& route : routes)
            if(route.id == routePick) preview = routeTitle(route);
        ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##plow_ed_route_pick", preview.c_str()))
        {
            if(ImGui::Selectable("Все маршруты города", routePick == "0")) Queue("ed_route:0");
            for(const auto& route : routes)
            {
                const std::string label = routeTitle(route) + "###plow_ed_r" + route.id;
                if(ImGui::Selectable(label.c_str(), route.id == routePick)) Queue("ed_route:" + route.id);
            }
            ImGui::EndCombo();
        }
        // «Создать маршрут»: город, потом маршрут — эдитор переходит на него, новые метки ложатся в него.
        if(ImGui::Button("Создать маршрут###plow_ed_new", ImVec2(-1, 38 * scale))) ImGui::OpenPopup("##plow_route_new");
        ImGui::EndDisabled();
        ImGui::SetNextWindowSize(ImVec2(700 * scale, 0));
        if(ImGui::BeginPopup("##plow_route_new"))
        {
            section("СОЗДАТЬ МАРШРУТ: ГОРОД");
            cityButtons("plow_new_city", 36);
            ImGui::Spacing();
            section(cityName.empty() ? "МАРШРУТ" : ("МАРШРУТ — " + cityName).c_str());
            for(const auto& route : routes)
            {
                const std::string label = routeTitle(route) + "###plow_new_r" + route.id;
                if(ImGui::Button(label.c_str(), ImVec2(-1, 34 * scale)))
                {
                    Queue("route_new:" + std::to_string(cityIndex) + ":" + route.id);
                    ImGui::CloseCurrentPopup();
                }
            }
            if(routes.empty())
                colored(muted, "Маршрутов города пока нет: сервер отдаёт их, когда берёшь работу в этом городе.");
            if(ImGui::Button("Без маршрута — метки по городу###plow_new_none", ImVec2(-1, 34 * scale)))
            {
                Queue("route_new:" + std::to_string(cityIndex) + ":0");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if(online && !state["ed_work"].empty())
        {
            // Идёт создание маршрута: новые метки ложатся в него. Нажать — закончить.
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(28, 78, 52, 255));
            const std::string work = "Создаю маршрут " + state["ed_work"] + "  /  готово###plow_ed_done";
            action(work.c_str(), "route_done", online);
            ImGui::PopStyleColor();
        }
        action("Выбрать ближайшую###plow_ed_near", "wp_select:near", online);
        ImGui::BeginChild("##plow_ed_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::PushFont(ImGui::GetFont(), 16 * scale);
        int count = 0;
        if(online)
        {
            // Точки города: ворота депо и финиш — общие для всех его маршрутов.
            bool header = false;
            for(const auto& item : split(state["ed_points"], ';'))
            {
                const auto row = split(item, ',');
                if(row.size() < 3 || row[0].empty()) continue;
                if(!header) { colored(muted, "Точки города"); header = true; }
                const bool chosen = cityPoint == row[0];
                ImGui::PushID(row[0].c_str());
                const std::string label = pointName(row[0]) + "  зона " + row[1] + " м  /  " + distanceText(row[2]);
                ImGui::PushStyleColor(ImGuiCol_Text, chosen ? amber : row[0] == "gate" ? pink : red);
                if(ImGui::Selectable(label.c_str(), chosen, 0, ImVec2(0, 28 * scale))) Queue("wp_select:" + row[0]);
                ImGui::PopStyleColor();
                ImGui::PopID();
                ++count;
            }
            header = false;
            for(const auto& item : split(state["wp_list"], ';'))
            {
                const auto row = split(item, ',');
                if(row.size() < 4 || row[0].empty()) continue;
                if(!header) { colored(muted, "Метки"); header = true; }
                const int index = std::atoi(row[0].c_str());
                const bool noRoute = row[3] == "-";
                ImGui::PushID(index);
                const std::string label = std::string(noRoute ? "! " : "") + "#" + row[0] + "  зона " + row[1]
                    + " м  /  " + distanceText(row[2]);
                ImGui::PushStyleColor(ImGuiCol_Text, index == selected ? amber : noRoute ? red : white);
                if(ImGui::Selectable(label.c_str(), index == selected, 0, ImVec2(0, 28 * scale)))
                    Queue("wp_select:" + row[0]);
                ImGui::PopStyleColor();
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("До метки: %s; маршрут: %s",
                    distanceText(row[2]).c_str(), noRoute ? "нет — бот берёт её на любом" : row[3].c_str());
                ImGui::PopID();
                ++count;
            }
            // Обученные светофоры города: «зелёное состояние,курс,расстояние». Их учат во вкладке «Метки».
            header = false;
            for(const auto& item : split(state["ed_lights"], ';'))
            {
                const auto row = split(item, ',');
                if(row.size() < 3 || row[0].empty()) continue;
                if(!header) { colored(muted, "Светофоры"); header = true; }
                colored(sky, "Светофор  зелёный = " + row[0] + "  /  курс " + row[1] + "°  /  " + distanceText(row[2]));
                ++count;
            }
        }
        if(count == 0) colored(muted, online ? "В этом городе пока ничего нет. Создай маршрут или метки во вкладке «Метки»."
            : "Бот не подключён");
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##plow_mark_edit", ImVec2(0, editorHeight), ImGuiChildFlags_None);
        section(cityPoint.empty() ? "ВЫБРАННАЯ МЕТКА" : "ВЫБРАННАЯ ТОЧКА ГОРОДА");
        ImGui::PushFont(ImGui::GetFont(), 28 * scale);
        const std::string title = !cityPoint.empty() ? cityName + ": " + pointName(cityPoint) + "  /  зона " + edit[4] + " м"
            : "#" + edit[0] + "  /  зона " + edit[4] + " м";
        colored(has ? amber : muted, has ? title : online ? "Выбери метку в списке" : "Бот не подключён");
        ImGui::PopFont();
        const bool noRoute = has && cityPoint.empty() && edit[5] == "-";
        if(has) colored(muted, "До неё " + distanceText(edit[6]) + "  /  курс " + edit[3]
            + (!cityPoint.empty() ? std::string("  /  общая для всех маршрутов города")
                : noRoute ? std::string() : "  /  маршрут " + edit[5]));
        if(noRoute && ImGui::BeginTable("##plow_ed_route", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            colored(red, "Без маршрута: едет на любом");
            ImGui::TableNextColumn();
            action("Привязать к маршруту###plow_ed_bind", "wp_route:bind", online);
            ImGui::EndTable();
        }
        static int stepIndex = 1;
        static const char* stepValue[4]{"0.25", "0.5", "1", "2"};
        ImGui::BeginDisabled(!has);
        if(ImGui::BeginTable("##plow_nudge", 3, ImGuiTableFlags_SizingStretchSame))
        {
            const auto nudge = [&](const char* label, const char* direction)
            {
                if(ImGui::Button(label, ImVec2(-1, 40 * scale)))
                    Queue(std::string("wp_nudge:") + direction + ":" + stepValue[stepIndex]);
            };
            ImGui::TableNextColumn(); ImGui::TableNextColumn(); nudge("Вперёд###plow_ed_fwd", "fwd");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); nudge("Влево###plow_ed_left", "left");
            ImGui::TableNextColumn(); colored(muted, "По камере");
            ImGui::TableNextColumn(); nudge("Вправо###plow_ed_right", "right");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TableNextColumn(); nudge("Назад###plow_ed_back", "back");
            ImGui::EndTable();
        }
        colored(muted, "Шаг перемещения");
        if(ImGui::BeginTable("##plow_steps", 4, ImGuiTableFlags_SizingStretchSame))
        {
            for(int i = 0; i < 4; ++i)
            {
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Button, stepIndex == i
                    ? IM_COL32(108, 46, 155, 255) : IM_COL32(51, 33, 73, 255));
                const std::string label = std::string(stepValue[i]) + " м###plow_ed_step" + std::to_string(i);
                if(ImGui::Button(label.c_str(), ImVec2(-1, 36 * scale))) stepIndex = i;
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        if(ImGui::BeginTable("##plow_radius", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn(); action("Зона  -0.5 м###plow_ed_smaller", "wp_radius:-0.5");
            ImGui::TableNextColumn(); action("Зона  +0.5 м###plow_ed_bigger", "wp_radius:+0.5");
            // Порядок — только у меток: точки города стоят в начале и в конце любого маршрута.
            ImGui::TableNextColumn(); action("Раньше по ходу###plow_ed_earlier", "wp_order:-1", selected > 0);
            ImGui::TableNextColumn(); action("Позже по ходу###plow_ed_later", "wp_order:1", selected > 0);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        const std::string undoLabel = "Отменить правку (" + std::to_string(undo) + ")###plow_ed_undo";
        action(undoLabel.c_str(), "wp_undo", online && undo > 0);
        ImGui::Spacing();
        section(cityName.empty() ? "ТОЧКИ ГОРОДА" : ("ТОЧКИ ГОРОДА — " + cityName).c_str());
        if(ImGui::BeginTable("##plow_city_points", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn(); action("Ворота депо здесь###plow_city_gate", "city_point:gate", online && !cityName.empty());
            ImGui::TableNextColumn(); action("Финиш здесь###plow_city_finish", "city_point:finish", online && !cityName.empty());
            ImGui::EndTable();
        }
        colored(muted, "Ставятся там, где стоит машина, с её курсом. Любой маршрут города выезжает "
            "через ворота, возвращается через них и встаёт на финише.");
        ImGui::Spacing();
        section("СОХРАНЕНИЕ");
        const std::string fileInfo = state["wp_file"].empty() ? "Изменений пока нет" : state["wp_file"];
        colored(fileInfo.find("не ") != std::string::npos ? red : mint, fileInfo);
        if(ImGui::CollapsingHeader("Как пользоваться редактором"))
        {
            colored(white, "Выбери метку и двигай её кнопками. Направления зависят от камеры. "
                "Зона задаёт, сколько места у бота для проезда. «Раньше/Позже по ходу» меняет порядок меток. "
                "Выбранная метка в мире — жёлтая; стрелка показывает направление при её создании.");
            colored(muted, "Правки сохраняются автоматически в PlowMarks.txt рядом с ботом. "
                "Резервная копия до первой правки за сессию — PlowMarks.bak.");
        }
        action("Выгрузить метки в лог###plow_ed_dump", "dump_waypoints", online);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleColor(19);
    ImGui::PopStyleVar(6);
    ImGui::PopFont();
}
