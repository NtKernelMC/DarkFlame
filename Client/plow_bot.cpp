#include "plow_bot.h"
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
    return flag("loaded") && (flag("bot") || flag("autonomy"));
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
        }
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

    static bool editorPage = false;
    static ULONGLONG editorTick{};
    if(online && (state["editor"] == "1") != editorPage && GetTickCount64() - editorTick > 700)
    {
        editorTick = GetTickCount64();
        Queue(editorPage ? "editor:1" : "editor:0");
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
    const ImU32 muted = IM_COL32(135, 126, 159, 255);
    const ImU32 violet = IM_COL32(190, 100, 255, 255);
    const ImU32 mint = IM_COL32(106, 234, 194, 255);
    const ImU32 amber = IM_COL32(255, 198, 109, 255);
    const ImU32 red = IM_COL32(255, 104, 141, 255);
    const ImU32 statusColor = !online ? muted : bot ? mint : violet;
    const float width = size.x / scale;
    constexpr float vertical = 1.22f;
    ImGui::SetCursorScreenPos(position);
    ImGui::PushFont(ImGui::GetFont(), 20 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 5 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 8 * scale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(9, 8, 17, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(51, 35, 74, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(39, 26, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(73, 39, 104, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(108, 46, 155, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(12, 10, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(49, 31, 70, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(70, 37, 100, 255));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, violet);
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, muted);
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(47, 33, 64, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(9, 8, 17, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(58, 43, 76, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(89, 54, 117, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(134, 67, 176, 255));
    ImGui::BeginChild("##plow_bot", size, ImGuiChildFlags_Borders);
    const auto origin = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto point = [&](float x, float y)
    {
        return ImVec2(origin.x + x * scale,
            origin.y + y * scale * vertical - ImGui::GetScrollY());
    };
    const auto at = [&](float x, float y)
    {
        ImGui::SetCursorPos(ImVec2(x * scale, y * scale * vertical));
    };
    const auto text = [&](float x, float y, std::string_view label, float pixels,
        ImU32 color, float wrap = 0.0f)
    {
        draw->AddText(ImGui::GetFont(), pixels * 1.15f * scale, point(x, y), color,
            label.data(), label.data() + label.size(), wrap * scale);
    };
    const auto card = [&](float x, float y, float w, float h)
    {
        draw->AddRectFilled(point(x, y), point(x + w, y + h), IM_COL32(18, 15, 29, 255), 8 * scale);
        draw->AddRect(point(x, y), point(x + w, y + h), IM_COL32(47, 33, 65, 255), 8 * scale);
        draw->AddLine(point(x + 12, y), point(x + 45, y), IM_COL32(163, 75, 224, 145), 2 * scale);
    };
    const auto button = [&](const char* label, const char* command, float x, float y,
        float w, float h, bool enabled)
    {
        at(x, y);
        ImGui::BeginDisabled(!enabled);
        if(ImGui::Button(label, ImVec2(w * scale, h * scale * vertical))) Queue(command);
        ImGui::EndDisabled();
    };
    const auto metric = [&](float x, float y, float w, std::string_view label,
        std::string_view value, ImU32 color)
    {
        draw->AddRectFilled(point(x, y), point(x + w, y + 62), IM_COL32(13, 11, 22, 255), 6 * scale);
        draw->AddRect(point(x, y), point(x + w, y + 62), IM_COL32(41, 29, 57, 255), 6 * scale);
        text(x + 10, y + 8, label, 11, muted);
        text(x + 10, y + 26, value, 22, color);
    };

    draw->AddRectFilledMultiColor(point(12, 2), point(width - 12, 3),
        IM_COL32(181, 76, 255, 0), IM_COL32(181, 76, 255, 0),
        IM_COL32(181, 76, 255, 180), IM_COL32(181, 76, 255, 180));
    text(18, 12, "ПОЛИВАЛКА", 28, white);
    text(218, 22, editorPage ? "// ЭДИТОР МЕТОК" : "// ДОРОЖНАЯ СЛУЖБА", 13, violet);
    if(!editorPage)
    {
        const std::string route = state["route"].empty() ? "Маршрут не выбран" : state["route"];
        text(470, 17, route, 18, white);
    }
    else
    {
        at(width - 580, 12);
        if(ImGui::Button("<  К боту###plow_close_editor", ImVec2(170 * scale, 34 * scale * vertical)))
            editorPage = false;
    }
    draw->AddCircleFilled(point(width - 169, 25), 3 * scale, statusColor);
    text(width - 158, 17, !online ? "OFFLINE" : bot ? "BOT ACTIVE" : "STANDBY", 16, statusColor);
    const std::string status = !online ? "Скрипт: " + loader.text
        : state["status"].empty() ? "Готов к запуску" : state["status"];
    text(18, 44, status, 12, muted);

    {
        const bool recording = state["recording"] == "1";
        ImGui::BeginDisabled(!online);
        if(recording)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 30, 56, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(146, 36, 68, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(168, 42, 78, 255));
        }
        at(width - 400, 12);
        if(ImGui::Button(recording ? "Остановить запись###plow_record"
            : "Записывать телеметрию###plow_record", ImVec2(222 * scale, 34 * scale * vertical)))
        {
            Queue(recording ? "record_stop" : "record_start");
        }
        if(recording) ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }

    if(editorPage)
    {
        struct MarkRow
        {
            int index{};
            std::string radius, distance, route;
        };
        std::vector<MarkRow> rows;
        const auto split = [](const std::string& value, char separator)
        {
            std::vector<std::string> parts;
            size_t from = 0;
            while(from <= value.size())
            {
                const auto next = value.find(separator, from);
                parts.push_back(value.substr(from, next == std::string::npos ? std::string::npos
                    : next - from));
                if(next == std::string::npos) break;
                from = next + 1;
            }
            return parts;
        };
        if(online)
        {
            for(const auto& item : split(state["wp_list"], ';'))
            {
                const auto parts = split(item, ',');
                if(parts.size() >= 4 && !parts[0].empty())
                    rows.push_back({std::atoi(parts[0].c_str()), parts[1], parts[2], parts[3]});
            }
        }
        // wp_edit: номер|x|y|курс|зона|маршрут|до неё|отмен
        auto edit = online ? split(state["wp_edit"], '|') : std::vector<std::string>{};
        edit.resize(8);
        const int selected = edit[0].empty() ? 0 : std::atoi(edit[0].c_str());
        const int undo = edit[7].empty() ? 0 : std::atoi(edit[7].c_str());
        const auto distanceText = [](const std::string& value)
        {
            return value.empty() || value[0] == '-' ? std::string("--") : value + " м";
        };

        constexpr float listX = 16, listW = 380;
        card(listX, 64, listW, 494);
        text(listX + 16, 79, "МЕТКИ", 12, muted);
        text(listX + listW - 80, 79, std::to_string(rows.size()) + " шт.", 12, amber);
        ImGui::BeginDisabled(!online);
        at(listX + 16, 100);
        if(ImGui::Button("Выбрать ближайшую###plow_ed_near",
            ImVec2((listW - 32) * scale, 32 * scale * vertical)))
        {
            Queue("wp_select:near");
        }
        ImGui::EndDisabled();
        at(listX + 16, 142);
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(108, 46, 155, 200));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(73, 39, 104, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(128, 56, 180, 255));
        ImGui::BeginChild("##plow_ed_list", ImVec2((listW - 32) * scale, 404 * scale * vertical));
        const float rowFont = 15 * 1.15f * scale;
        for(const auto& row : rows)
        {
            ImGui::PushID(row.index);
            const bool active = row.index == selected;
            if(ImGui::Selectable("##plow_ed_row", active, 0, ImVec2(0, 30 * scale)))
                Queue("wp_select:" + std::to_string(row.index));
            const ImVec2 min = ImGui::GetItemRectMin();
            ImDrawList* rowDraw = ImGui::GetWindowDrawList();
            const ImU32 color = active ? amber : white;
            const std::string number = "#" + std::to_string(row.index);
            const std::string zone = "зона " + row.radius + " м";
            const std::string away = distanceText(row.distance);
            rowDraw->AddText(ImGui::GetFont(), rowFont, ImVec2(min.x + 10 * scale, min.y + 5 * scale),
                color, number.c_str());
            rowDraw->AddText(ImGui::GetFont(), rowFont, ImVec2(min.x + 70 * scale, min.y + 5 * scale),
                color, zone.c_str());
            rowDraw->AddText(ImGui::GetFont(), rowFont, ImVec2(min.x + 220 * scale, min.y + 5 * scale),
                muted, away.c_str());
            ImGui::PopID();
        }
        if(rows.empty())
            ImGui::TextDisabled(online ? "Меток нет: создай их на странице бота" : "Бот не подключён");
        ImGui::EndChild();
        ImGui::PopStyleColor(3);

        const float editX = listX + listW + 16;
        constexpr float editW = 440;
        card(editX, 64, editW, 494);
        text(editX + 16, 79, "ВЫБРАННАЯ МЕТКА", 12, muted);
        const bool has = online && selected > 0;
        if(has)
        {
            text(editX + 16, 98, "#" + edit[0], 34, amber);
            text(editX + 150, 106, "зона " + edit[4] + " м", 22, white);
            text(editX + 16, 148, "до неё " + distanceText(edit[6]) + "   курс " + edit[3]
                + "   маршрут " + (edit[5] == "-" ? std::string("любой") : edit[5]), 12, muted,
                editW - 32);
        }
        else
        {
            text(editX + 16, 106, online ? "Выбери метку в списке" : "Бот не подключён", 18, muted);
        }
        static int stepIndex = 1;
        static const char* stepValue[4]{"0.25", "0.5", "1", "2"};
        const auto nudge = [&](const char* label, const char* direction, float x, float y)
        {
            at(x, y);
            if(ImGui::Button(label, ImVec2(128 * scale, 40 * scale * vertical)))
                Queue(std::string("wp_nudge:") + direction + ":" + stepValue[stepIndex]);
        };
        ImGui::BeginDisabled(!has);
        const float padX = editX + editW / 2 - 64;
        nudge("Вперёд###plow_ed_fwd", "fwd", padX, 176);
        nudge("Влево###plow_ed_left", "left", padX - 138, 222);
        nudge("Вправо###plow_ed_right", "right", padX + 138, 222);
        nudge("Назад###plow_ed_back", "back", padX, 268);
        text(editX + 16, 324, "ШАГ", 11, muted);
        for(int i = 0; i < 4; ++i)
        {
            at(editX + 64 + static_cast<float>(i) * 92, 320);
            const std::string label = std::string(stepValue[i]) + " м###plow_ed_step" + std::to_string(i);
            ImGui::RadioButton(label.c_str(), &stepIndex, i);
        }
        text(editX + 16, 362, "ЗОНА", 11, muted);
        const float halfW = (editW - 42) / 2;
        at(editX + 16, 380);
        if(ImGui::Button("Меньше  -0.5 м###plow_ed_smaller", ImVec2(halfW * scale, 40 * scale * vertical)))
            Queue("wp_radius:-0.5");
        at(editX + 26 + halfW, 380);
        if(ImGui::Button("Больше  +0.5 м###plow_ed_bigger", ImVec2(halfW * scale, 40 * scale * vertical)))
            Queue("wp_radius:+0.5");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!online || undo == 0);
        at(editX + 16, 434);
        const std::string undoLabel = "Отменить последнюю правку (" + std::to_string(undo)
            + ")###plow_ed_undo";
        if(ImGui::Button(undoLabel.c_str(), ImVec2((editW - 32) * scale, 36 * scale * vertical)))
            Queue("wp_undo");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!has);
        at(editX + 16, 478);
        if(ImGui::Button("Раньше по ходу###plow_ed_earlier", ImVec2(halfW * scale, 36 * scale * vertical)))
            Queue("wp_order:-1");
        at(editX + 26 + halfW, 478);
        if(ImGui::Button("Позже по ходу###plow_ed_later", ImVec2(halfW * scale, 36 * scale * vertical)))
            Queue("wp_order:1");
        ImGui::EndDisabled();
        text(editX + 16, 526, "Направления — как смотрит камера.", 11, muted, editW - 32);

        const float helpX = editX + editW + 16;
        const float helpW = width - helpX - 16;
        card(helpX, 64, helpW, 494);
        text(helpX + 16, 79, "КАК ПОЛЬЗОВАТЬСЯ", 12, muted);
        text(helpX + 16, 102, "1. Выбери метку в списке или нажми «Выбрать ближайшую».\n"
            "2. Двигай кнопками — метка переезжает в мире сразу.\n"
            "3. Меняй зону: чем она больше, тем больше у бота места выбрать, где проехать.\n"
            "4. Номер — порядок проезда. Новая метка сама встаёт по ходу маршрута; "
            "поправить — «Раньше/Позже по ходу».\n\n"
            "Пока открыт эдитор, метки видно в мире и без отладки: над каждой номер и зона, "
            "выбранная — жёлтая, стрелка — куда ты ехал, когда её ставил.", 12, white, helpW - 32);
        text(helpX + 16, 326, "ФАЙЛ МЕТОК", 12, muted);
        const std::string fileInfo = state["wp_file"].empty() ? "--" : state["wp_file"];
        text(helpX + 16, 348, fileInfo, 12,
            fileInfo.find("не ") != std::string::npos ? red : mint, helpW - 32);
        text(helpX + 16, 414, "Каждая правка сохраняется сама в PlowMarks.txt рядом с ботом. "
            "Копия файла до первой правки за сессию — PlowMarks.bak.", 11, muted, helpW - 32);
        button("Выгрузить в лог###plow_ed_dump", "dump_waypoints", helpX + 16, 500, helpW - 32, 34, online);
    }
    else
    {
        constexpr float leftX = 16, leftW = 300;
        card(leftX, 64, leftW, 240);
        text(leftX + 16, 79, "УПРАВЛЕНИЕ", 12, muted);
        text(leftX + 206, 79, bot ? "РАБОТАЕТ" : ready ? "ГОТОВ" : "ЖДЁТ", 11, statusColor);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(79, 24, 50, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(95, 29, 59, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(108, 33, 66, 255));
        button(bot ? "Остановить бота###plow_bot" : "Запустить бота###plow_bot",
            bot ? "bot_stop" : "bot_start", leftX + 16, 104, leftW - 32, 46, ready);
        ImGui::PopStyleColor(3);
        ImGui::BeginDisabled(!online);
        at(leftX + 16, 164);
        bool autonomy = state["autonomy"] == "1";
        if(ImGui::Checkbox("Автономность###plow_autonomy", &autonomy))
            Queue(autonomy ? "autonomy:1" : "autonomy:0");
        at(leftX + 160, 164);
        bool debug = state["debug"] == "1";
        if(ImGui::Checkbox("Отладка###plow_debug", &debug))
            Queue(debug ? "debug:1" : "debug:0");

        text(leftX + 16, 206, "ОГРАНИЧИТЕЛЬ, ПО СПИДОМЕТРУ", 11, muted);
        static int limit = 50;
        static std::string lastLimit;
        if(lastLimit != state["speed_limit"] && !state["speed_limit"].empty())
        {
            lastLimit = state["speed_limit"];
            limit = std::atoi(lastLimit.c_str());
        }
        at(leftX + 16, 224);
        ImGui::SetNextItemWidth((leftW - 32) * scale);
        if(ImGui::SliderInt("##plow_limit", &limit, 20, 90, "%d км/ч"))
        {
            Queue("speed_limit:" + std::to_string(limit));
            lastLimit = std::to_string(limit);
        }
        ImGui::EndDisabled();
        text(leftX + 16, 262, spray ? "Установка включена" : "Установка выключена", 12,
            spray ? mint : muted);

        card(leftX, 316, leftW, 242);
        text(leftX + 16, 331, "СКРИПТ БОТА", 12, muted);
        const ImU32 loadColor = loader.kind == PlowLoad::Loaded ? mint
            : loader.kind == PlowLoad::Error ? red
            : loader.kind == PlowLoad::Waiting ? muted : amber;
        const char* loadTag = loader.kind == PlowLoad::Loaded ? "АВТО"
            : loader.kind == PlowLoad::Error ? "ОШИБКА"
            : loader.kind == PlowLoad::Pending ? "ОБНОВЛЕНИЕ"
            : loader.kind == PlowLoad::Unloaded ? "ВЫГРУЖЕН" : "ЖДЁТ";
        text(leftX + 176, 331, loadTag, 11, loadColor);
        const std::string script = loader.version.empty() ? std::string("PlowBot.lua не прочитан")
            : "PlowBot " + loader.version + "  ·  " + std::to_string((loader.bytes + 512) / 1024) + " КБ";
        text(leftX + 16, 352, script, 16, white);
        text(leftX + 16, 382, loader.text, 11, loadColor, leftW - 32);
        at(leftX + 16, 462);
        bool autoReload = g_autoReload.load();
        if(ImGui::Checkbox("Подхватывать новую версию###plow_auto_reload", &autoReload))
            g_autoReload.store(autoReload);
        at(leftX + 16, 500);
        if(ImGui::Button(loader.kind == PlowLoad::Unloaded ? "Загрузить скрипт###plow_reload"
            : "Перезагрузить скрипт###plow_reload", ImVec2((leftW - 32) * scale, 40 * scale * vertical)))
        {
            g_reloadRequest.store(true);
        }

        const float midX = leftX + leftW + 16;
        constexpr float midW = 336;
        card(midX, 64, midW, 494);
        text(midX + 16, 79, "СВЕТОФОРЫ", 12, muted);
        const bool lightsWork = state["traffic_working"] == "1";
        const std::string lightNow = state["traffic_state"].empty() ? "--" : state["traffic_state"];
        text(midX + midW - 60, 79, lightNow, 12, lightsWork ? amber : muted);
        ImGui::BeginDisabled(!online);
        at(midX + 16, 100);
        bool ignoreTraffic = state["ignore_traffic"] == "1";
        if(ImGui::Checkbox("Игнорировать светофоры###plow_ignore_lights", &ignoreTraffic))
            Queue(ignoreTraffic ? "ignore_traffic:1" : "ignore_traffic:0");
        ImGui::EndDisabled();
        text(midX + 16, 136, lightsWork ? "Светофоры работают"
            : "Светофоры не работают — бот их не ждёт", 11, lightsWork ? mint : muted,
            midW - 32);
        button("Тест светофора###plow_light_test", "test_traffic",
            midX + 16, 158, midW - 32, 36, online);
        button("Сохранить (на зелёный)###plow_light_save", "save_traffic",
            midX + 16, 200, midW - 32, 36, online);
        button("Забыть ближайший###plow_light_forget", "forget_traffic",
            midX + 16, 242, midW - 32, 36, online);
        button("Выгрузить в лог###plow_light_dump", "dump_traffic",
            midX + 16, 284, midW - 32, 36, online);
        const std::string trafficInfo = state["traffic_info"].empty()
            ? "Светофоры не обучены" : state["traffic_info"];
        text(midX + 16, 330, trafficInfo, 11, muted, midW - 32);

        draw->AddLine(point(midX + 16, 356), point(midX + midW - 16, 356),
            IM_COL32(47, 33, 65, 255), 1 * scale);
        text(midX + 16, 364, "СОЗДАТЬ МЕТКУ", 12, mint);
        const std::string waypoints = state["waypoints"].empty() ? "0" : state["waypoints"];
        const std::string wpCursor = state["waypoint_cursor"].empty() ? "1" : state["waypoint_cursor"];
        text(midX + midW - 96, 364, "проезд " + wpCursor + " из " + waypoints, 11, amber);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(28, 78, 52, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(36, 98, 66, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(44, 120, 80, 255));
        const float third = (midW - 52) / 3;
        button("Малая###plow_wp_small", "save_waypoint:small",
            midX + 16, 386, third, 30, online);
        button("Средняя###plow_wp_mid", "save_waypoint:mid",
            midX + 26 + third, 386, third, 30, online);
        button("Большая###plow_wp_big", "save_waypoint:big",
            midX + 36 + third * 2, 386, third, 30, online);
        ImGui::PopStyleColor(3);

        const std::string lastWp = state["waypoint_last"].empty() ? "--" : state["waypoint_last"];
        text(midX + 16, 424, "Размер метки #" + lastWp, 11, muted);
        static int wpSize = 14;
        static std::string lastWpSize;
        if(lastWpSize != state["waypoint_size"] && !state["waypoint_size"].empty())
        {
            lastWpSize = state["waypoint_size"];
            wpSize = std::atoi(lastWpSize.c_str());
        }
        ImGui::BeginDisabled(!online || lastWp == "--");
        at(midX + 16, 442);
        ImGui::SetNextItemWidth((midW - 32) * scale);
        if(ImGui::SliderInt("##plow_wp_size", &wpSize, 2, 50, "%d м"))
        {
            Queue("waypoint_size:" + std::to_string(wpSize));
            lastWpSize = std::to_string(wpSize);
        }
        ImGui::EndDisabled();
        const float half = (midW - 42) / 2;
        button("Убрать ближайшую###plow_wp_forget", "forget_waypoint",
            midX + 16, 480, half, 30, online);
        button("Выгрузить в лог###plow_wp_dump", "dump_waypoints",
            midX + 26 + half, 480, half, 30, online);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(28, 78, 52, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(36, 98, 66, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(44, 120, 80, 255));
        at(midX + 16, 518);
        if(ImGui::Button("Эдитор меток  >###plow_open_editor",
            ImVec2((midW - 32) * scale, 30 * scale * vertical)))
        {
            editorPage = true;
        }
        ImGui::PopStyleColor(3);

        const float rightX = midX + midW + 16;
        const float rightW = width - rightX - 16;
        const float cell = (rightW - 52) / 3;
        card(rightX, 64, rightW, 494);
        text(rightX + 16, 79, "ТЕЛЕМЕТРИЯ", 12, muted);
        static const char* labels[12]{"СКОРОСТЬ", "ТОЧКА", "ВОДА", "СВЕТОФОР",
            "ДО ТОЧКИ", "ЗАПАС ВОДЫ", "РУЛЬ", "ГАЗ", "ПОМЕХА",
            "ВОДА, Л", "ЗАСТРЯЛ", "РЕЙСОВ"};
        for(int i = 0; i < 12; ++i)
        {
            const float mx = rightX + 16 + static_cast<float>(i % 3) * (cell + 10);
            const float my = 100 + static_cast<float>(i / 3) * 72;
            ImU32 color = white;
            if(values[i] == "--") color = muted;
            else if(i == 0) color = mint;
            else if(i == 2) color = violet;
            else if(i == 3)
            {
                color = values[3].rfind("КРАС", 0) == 0 ? red
                    : values[3].rfind("ЗЕЛ", 0) == 0 ? mint : muted;
            }
            else if(i == 5 && !values[5].empty() && values[5][0] == '-') color = red;
            metric(mx, my, cell, labels[i], values[i], color);
        }

    }

    const float reportY = 570;
    const float reportH = std::max(110.0f, size.y / (scale * vertical) - reportY - 16);
    card(16, reportY, width - 32, reportH);
    text(32, reportY + 15, "ОТЧЁТ БОТА", 12, muted);
    if(!error.empty()) text(220, reportY + 15, error, 12, red, width - 260);
    else text(220, reportY + 15, path, 11, muted, width - 260);
    const std::string report = state["report"].empty()
        ? "Бот ещё не присылал отчёт." : state["report"];
    text(32, reportY + 38, report, 13, white, width - 64);

    ImGui::EndChild();
    ImGui::PopStyleColor(16);
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
}
