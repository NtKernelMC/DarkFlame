#include "pilot_telemetry.h"
#include "embedded_lua_runtime.h"
#include "../Shared/runtime_log.h"

#include <deque>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace
{
std::mutex g_mutex;
std::map<std::string, std::string> g_state;
std::deque<std::string> g_commands;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::string g_path;
std::wstring g_soundDirectory;
ULONGLONG g_heartbeat{};
unsigned long long g_generation{};
std::string g_lease;
char g_note[192]{};
int g_rate = 1;

struct SoundPlayer
{
    std::mutex mutex;
    HANDLE wake{}, thread{};
    int pending{};
    unsigned long long requested{};
    std::wstring directory;
    std::string lease;
};

SoundPlayer& Audio()
{
    static auto* player = new SoundPlayer;
    return *player;
}

DWORD WINAPI PlayPilotSounds(void*)
{
    auto& player = Audio();
    MCIDEVICEID device{};
    for(;;)
    {
        WaitForSingleObject(player.wake, INFINITE);
        int cue{};
        unsigned long long request{};
        std::wstring directory;
        std::string lease;
        {
            std::scoped_lock lock(player.mutex);
            cue = player.pending;
            player.pending = 0;
            request = player.requested;
            directory = player.directory;
            lease = player.lease;
        }
        if(!cue) continue;
        if(device) mciSendCommandW(device, MCI_CLOSE, MCI_WAIT, 0);
        device = 0;
        const wchar_t* filename = cue == 1 ? L"AutoPilotON.mp3" : L"AirbusOff.mp3";
        const std::wstring path = directory + filename;
        MCI_OPEN_PARMSW open{};
        open.lpstrDeviceType = L"mpegvideo";
        open.lpstrElementName = path.c_str();
        MCIERROR error = mciSendCommandW(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_WAIT,
            reinterpret_cast<DWORD_PTR>(&open));
        if(!error) device = open.wDeviceID;
        bool current;
        {
            std::scoped_lock lock(player.mutex);
            current = request == player.requested;
        }
        if(!current) continue;
        if(!error)
        {
            MCI_PLAY_PARMS play{};
            error = mciSendCommandW(device, MCI_PLAY, MCI_FROM, reinterpret_cast<DWORD_PTR>(&play));
        }
        {
            std::scoped_lock lock(g_mutex);
            if(lease == g_lease)
                g_state["autopilot_sound_error"] = error ? "Не удалось воспроизвести "
                    + std::string(cue == 1 ? "AutoPilotON.mp3" : "AirbusOff.mp3")
                    + " (MCI " + std::to_string(error) + ")" : "";
        }
    }
}

bool QueuePilotSound(std::string_view value)
{
    if(value != "on" && value != "off") return false;
    auto& player = Audio();
    std::scoped_lock lock(player.mutex);
    if(!player.thread)
    {
        player.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if(player.wake) player.thread = CreateThread(nullptr, 0, &PlayPilotSounds, nullptr, 0, nullptr);
        if(!player.thread)
        {
            const DWORD error = GetLastError();
            if(player.wake) CloseHandle(player.wake);
            player.wake = nullptr;
            g_state["autopilot_sound_error"] = "Не удалось запустить звук: " + std::to_string(error);
            return false;
        }
    }
    player.pending = value == "on" ? 1 : 2;
    ++player.requested;
    player.directory = g_soundDirectory;
    player.lease = g_lease;
    SetEvent(player.wake);
    return true;
}

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
    ULONGLONG lastIoMs{}, maxIoMs{};
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
        const ULONGLONG began = GetTickCount64();
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
            if(!batches.empty() || flushed)
            {
                writer.lastIoMs = GetTickCount64() - began;
                if(writer.lastIoMs > writer.maxIoMs) writer.maxIoMs = writer.lastIoMs;
            }
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
    if(text.size() > 4 * 1024 * 1024 - writer.pendingBytes || writer.submitted - writer.completed >= 2048)
    {
        writer.error = "Log queue full (4 MB): recording stopped; accepted data is still draining";
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

int PilotLog(void* lua)
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

int PilotUpdate(void* lua)
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
    if(name == "play_sound")
    {
        DarkFlameLuaPushBoolean(lua, QueuePilotSound(std::string_view(value, valueSize)));
        return 1;
    }
    if(!CurrentCollector(lua, 3))
    {
        DarkFlameLuaPushBoolean(lua, false);
        return 1;
    }
    if(g_state.size() >= 32 && !g_state.contains(std::string(key, keySize))) return 0;
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

int PilotTakeCommand(void* lua)
{
    std::scoped_lock lock(g_mutex);
    if(!CurrentCollector(lua, 1)) return 0;
    if(GetTickCount64() - g_heartbeat > 2500) g_commands.clear();
    if(g_commands.empty()) return 0;
    DarkFlameLuaPushString(lua, g_commands.front().c_str());
    g_commands.pop_front();
    return 1;
}

void Queue(std::string command)
{
    std::scoped_lock lock(g_mutex);
    if(g_commands.size() < 16) g_commands.push_back(std::move(command));
}

void Button(const char* label, const char* command)
{
    if(ImGui::Button(label)) Queue(command);
}
}

void InitializePilotTelemetry()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        std::scoped_lock lock(g_mutex);
        std::wstring path = RuntimeLog::Path();
        const auto slash = path.find_last_of(L"\\/");
        path.resize(slash == std::wstring::npos ? 0 : slash + 1);
        g_soundDirectory = path;
        path += L"PilotTelemetry.log";
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
        sprintf_s(header, "{\"type\":\"game_start\",\"schema\":1,\"pid\":%lu,"
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

void RegisterPilotTelemetryLua(void* lua)
{
    DarkFlameLuaRegister(lua, "dfPilotLog", &PilotLog);
    DarkFlameLuaRegister(lua, "dfPilotUpdate", &PilotUpdate);
    DarkFlameLuaRegister(lua, "dfPilotTakeCommand", &PilotTakeCommand);
}

void DrawPilotTelemetry(ImVec2 position, ImVec2 size, float scale)
{
    std::map<std::string, std::string> state;
    std::string error, path;
    ULONGLONG age{};
    unsigned long long bytes{};
    size_t pendingBytes{};
    bool draining{};
    ULONGLONG lastIoMs{}, maxIoMs{};
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
        bytes = writer.bytes;
        pendingBytes = writer.pendingBytes;
        draining = writer.submitted != writer.flushed;
        lastIoMs = writer.lastIoMs;
        maxIoMs = writer.maxIoMs;
    }
    const bool online = state["loaded"] == "1" && age < 2500;
    const bool ready = online && state["resource_ready"] == "1";
    const bool recording = online && state["recording"] == "1" && error.empty();
    const bool autopilot = online && state["autopilot"] == "1";
    const bool waiting = online && state["autopilot_waiting"] == "1";
    if(state["interval_ms"] == "20") g_rate = 0;
    else if(state["interval_ms"] == "50") g_rate = 1;
    else if(state["interval_ms"] == "100") g_rate = 2;
    else if(state["interval_ms"] == "200") g_rate = 3;

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
    const ImU32 red = IM_COL32(255, 104, 141, 255);
    const ImU32 statusColor = !online ? muted : autopilot ? mint : violet;
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
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(40, 25, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(64, 34, 91, 255));
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(47, 33, 64, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(77, 41, 105, 255));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 15, 29, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(9, 8, 17, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(58, 43, 76, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(89, 54, 117, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(134, 67, 176, 255));
    ImGui::BeginChild("##pilot_telemetry", size, ImGuiChildFlags_Borders);
    const auto origin = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto point = [&](float x, float y)
    {
        return ImVec2(origin.x + x * scale, origin.y + y * scale * vertical - ImGui::GetScrollY());
    };
    const auto at = [&](float x, float y) { ImGui::SetCursorPos(ImVec2(x * scale, y * scale * vertical)); };
    const auto text = [&](float x, float y, std::string_view label, float pixels, ImU32 color, float wrap = 0.0f)
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
    const auto button = [&](const char* label, const char* command, float x, float y, float w, float h, bool enabled)
    {
        at(x, y);
        ImGui::BeginDisabled(!enabled);
        if(ImGui::Button(label, ImVec2(w * scale, h * scale * vertical))) Queue(command);
        ImGui::EndDisabled();
    };
    draw->AddRectFilledMultiColor(point(12, 2), point(width - 12, 3), IM_COL32(181, 76, 255, 0),
        IM_COL32(181, 76, 255, 0), IM_COL32(181, 76, 255, 180), IM_COL32(181, 76, 255, 180));
    text(18, 12, "PILOT", 28, white);
    text(116, 22, "// FLIGHT CONTROL", 13, violet);
    const std::string destination = state["destination"].empty() ? "Маршрут не назначен" : state["destination"];
    text(374, 17, destination, 18, white);
    draw->AddCircleFilled(point(width - 159, 25), 3 * scale, statusColor);
    text(width - 148, 17, !online ? "OFFLINE" : autopilot ? "AP ACTIVE" : "STANDBY", 16, statusColor);
    const std::string status = !online ? "Ожидание подключения" : state["autopilot_status"].empty() ? "Готов к запуску" : state["autopilot_status"];
    text(18, 44, status, 12, muted);

    constexpr float left = 320;
    const float rightX = left + 28;
    const float rightWidth = width - rightX - 16;
    const float metricWidth = (rightWidth - 30) / 4;
    card(16, 64, left, 210);
    text(32, 79, "АВТОПИЛОТ", 12, muted);
    text(255, 79, autopilot ? "ENGAGED" : ready ? "READY" : "WAITING", 11, statusColor);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(79, 24, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(95, 29, 59, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(108, 33, 66, 255));
    button(autopilot || waiting ? "Остановить автопилот###pilot_autopilot" : "Запустить автопилот###pilot_autopilot",
        autopilot || waiting ? "autopilot_stop" : "autopilot_start", 32, 104, left - 32, 44, ready || autopilot || waiting);
    ImGui::PopStyleColor(3);
    button("Трудоустроиться", "accept_job", 32, 154, left - 32, 28, ready && !waiting);
    ImGui::BeginDisabled(!online);
    bool autonomy = state["autopilot_autonomy"] == "1";
    bool autopilotTelemetry = state["autopilot_telemetry"] == "1";
    bool autopilotHud = state["autopilot_hud"] == "1";
    at(32, 188);
    if(ImGui::Checkbox("Автономность (BETA)", &autonomy)) Queue(autonomy ? "autopilot_autonomy:1" : "autopilot_autonomy:0");
    at(32, 215);
    if(ImGui::Checkbox("Телеметрия автопилота", &autopilotTelemetry)) Queue(autopilotTelemetry ? "autopilot_telemetry:1" : "autopilot_telemetry:0");
    at(32, 242);
    if(ImGui::Checkbox("Отладочный HUD", &autopilotHud)) Queue(autopilotHud ? "autopilot_hud:1" : "autopilot_hud:0");
    ImGui::EndDisabled();

    const char* metricNames[]{"СКОРОСТЬ", "КУРС", "ВЫСОТА", "ВЕРТ. СКОРОСТЬ"};
    const char* metricUnits[]{"км/ч", "град", "м", "м/с"};
    for(int i = 0; i < 4; ++i)
    {
        const float x = rightX + i * (metricWidth + 10);
        card(x, 64, metricWidth, 80);
        text(x + 14, 77, metricNames[i], 11, muted);
        text(x + 13, 98, values[i], 30, i == 0 ? violet : white);
        text(x + metricWidth - 51, 112, metricUnits[i], 12, muted);
    }

    const float navWidth = rightWidth * 0.47f;
    const float planeX = rightX + navWidth + 10;
    card(rightX, 154, navWidth, 120);
    card(planeX, 154, rightWidth - navWidth - 10, 120);
    text(rightX + 14, 166, "НАВЕДЕНИЕ", 11, muted);
    const ImVec2 center = point(rightX + 59, 222);
    draw->AddCircle(center, 31 * scale, IM_COL32(78, 47, 110, 255), 40);
    draw->AddCircle(center, 19 * scale, IM_COL32(43, 29, 65, 255), 32);
    draw->AddLine(point(rightX + 22, 222), point(rightX + 96, 222), IM_COL32(58, 36, 82, 255));
    draw->AddLine(point(rightX + 59, 185), point(rightX + 59, 259), IM_COL32(58, 36, 82, 255));
    draw->AddTriangleFilled(point(rightX + 59, 216), point(rightX + 55, 228), point(rightX + 63, 228), white);
    char* turnEnd{};
    const float turn = std::strtof(values[7].c_str(), &turnEnd);
    if(turnEnd != values[7].c_str() && std::isfinite(turn))
    {
        const float radians = turn * 0.0174532925f;
        const ImVec2 target(center.x + std::sin(radians) * 31 * scale, center.y - std::cos(radians) * 31 * scale);
        draw->AddLine(center, target, IM_COL32(191, 102, 255, 120), scale);
        draw->AddCircleFilled(target, 4 * scale, violet);
        draw->AddCircle(target, 8 * scale, IM_COL32(181, 77, 255, 70), 16, 2 * scale);
    }
    text(rightX + 111, 192, values[7] + "°", 26, white);
    text(rightX + 111, 229, "ДОВОРОТ", 10, muted);
    text(rightX + navWidth * 0.64f, 194, values[8] + " м", 21, violet);
    text(rightX + navWidth * 0.64f, 230, values[11] + "  /  DZ " + values[15], 11, muted);
    text(planeX + 14, 166, "СОСТОЯНИЕ БОРТА", 11, muted);
    text(planeX + 14, 191, "AGL  " + values[4] + " м", 18, white);
    text(planeX + 14, 224, "ТАНГАЖ " + values[5] + "°   КРЕН " + values[6] + "°", 13, muted);
    text(planeX + 14, 250, "Шасси: " + values[9], 12, violet);
    text(planeX + 280, 191, values[10], 13, mint);
    text(planeX + 280, 221, "ГАЗ " + values[13] + "%", 12, muted);
    text(planeX + 280, 247, "ТОРМОЗ " + values[14] + "%", 12, muted);

    const float recorderWidth = (width - 44) * 0.53f;
    const float noticeX = recorderWidth + 28;
    card(16, 284, recorderWidth, 112);
    card(noticeX, 284, width - noticeX - 16, 112);
    text(32, 297, "РЕГИСТРАТОР", 11, muted);
    draw->AddCircleFilled(point(196, 303), 3 * scale, recording ? red : muted);
    text(207, 296, recording ? "REC" : "IDLE", 12, recording ? red : muted);
    char disk[128]{};
    std::snprintf(disk, sizeof(disk), "%.2f MB  /  %s", bytes / 1048576.0,
        !error.empty() ? "ОШИБКА" : draining ? "ЗАПИСЬ..." : "СОХРАНЕНО");
    text(recorderWidth - 247, 298, disk, 12, !error.empty() ? red : muted);
    button(recording ? "Остановить запись###pilot_record" : "Начать запись###pilot_record",
        recording ? "stop" : "start", 32, 321, 220, 29, ready && error.empty());
    at(268, 322);
    ImGui::BeginDisabled(!online);
    ImGui::SetNextItemWidth(108 * scale);
    if(ImGui::Combo("##pilot_rate", &g_rate, "50 Гц\0" "20 Гц\0" "10 Гц\0" "5 Гц\0"))
    {
        constexpr int intervals[]{20, 50, 100, 200};
        Queue("interval:" + std::to_string(intervals[g_rate]));
    }
    ImGui::EndDisabled();
    text(396, 329, (state["samples"].empty() ? "--" : state["samples"]) + " отсчётов", 13, muted);
    ImGui::BeginDisabled(!recording);
    at(32, 360);
    ImGui::SetNextItemWidth((recorderWidth - 142) * scale);
    ImGui::InputTextWithHint("##pilot_note", "Метка полёта...", g_note, sizeof(g_note));
    button("В лог", (std::string("note:") + g_note).c_str(), recorderWidth - 98, 360, 98, 26, recording);
    ImGui::EndDisabled();
    text(noticeX + 14, 297, "ЗАДАЧА РЕЙСА", 11, violet);
    text(width - 110, 297, "MSG " + (state["notification_count"].empty() ? "--" : state["notification_count"]), 11, muted);
    std::string notification = state["notification"];
    if(const auto newline = notification.find('\n'); newline != std::string::npos) notification.erase(0, newline + 1);
    if(notification.empty()) notification = "Ожидание задания";
    draw->PushClipRect(point(noticeX + 14, 320), point(width - 28, 383), true);
    text(noticeX + 14, 322, notification, 16, white, width - noticeX - 44);
    draw->PopClipRect();
    at(noticeX + 14, 320);
    ImGui::InvisibleButton("##pilot_notification", ImVec2((width - noticeX - 44) * scale, 63 * scale * vertical));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", notification.c_str());

    std::string failure = error.empty() ? "" : "Лог: " + error;
    const char* errorKeys[]{"collector_error", "autopilot_hud_error", "autopilot_sound_error", "observer_error"};
    const char* errorLabels[]{"Телеметрия: ", "HUD: ", "Звук: ", "Уведомления: "};
    for(int i = 0; i < 4; ++i)
        if(failure.empty() && !state[errorKeys[i]].empty()) failure = errorLabels[i] + state[errorKeys[i]];
    float extraY = 408;
    if(!failure.empty())
    {
        at(20, extraY);
        ImGui::PushStyleColor(ImGuiCol_Text, red);
        ImGui::PushTextWrapPos((width - 26) * scale);
        ImGui::TextUnformatted(failure.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        extraY = ImGui::GetCursorPosY() / (scale * vertical) + 4;
    }
    at(16, extraY);
    ImGui::SetNextItemWidth((width - 32) * scale);
    if(ImGui::CollapsingHeader("Диагностика полёта"))
    {
        if(ImGui::BeginTable("##pilot_readings", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX))
        {
            const char* headings[]{"Самолёт", "Ориентир", "Управление"};
            const char* keys[]{"flight", "target", "controls"};
            for(int i = 0; i < 3; ++i)
            {
                ImGui::TableNextColumn();
                ImGui::SeparatorText(headings[i]);
                ImGui::TextWrapped("%s", online ? state[keys[i]].c_str() : "—");
            }
            ImGui::EndTable();
        }
        ImGui::BeginDisabled(!recording);
        Button("Руление", "phase:taxi"); ImGui::SameLine();
        Button("Взлёт", "phase:takeoff"); ImGui::SameLine();
        Button("Полёт", "phase:cruise"); ImGui::SameLine();
        Button("Посадка", "phase:landing");
        ImGui::EndDisabled();
        ImGui::TextWrapped("Лог: %s", path.c_str());
        ImGui::Text("Очередь %.1f КБ | I/O %llu мс (max %llu) | ответ Lua %llu мс",
            pendingBytes / 1024.0, lastIoMs, maxIoMs, age);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(20);
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
}
