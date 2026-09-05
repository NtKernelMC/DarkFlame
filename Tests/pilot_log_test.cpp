#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include <vector>

namespace
{
HANDLE ioEntered{}, ioRelease{};
std::atomic<bool> stallWrite{}, stallFlush{}, failWrite{};
HANDLE audioEntered{}, audioRelease{};
std::atomic<bool> stallAudio{}, failAudio{};
std::mutex audioMutex;
std::vector<std::wstring> audioOpened, audioPlayed;
std::wstring audioCurrent;

MCIERROR WINAPI TestMciSendCommandW(MCIDEVICEID, UINT message, DWORD_PTR flags, DWORD_PTR parameters)
{
    if(message == MCI_OPEN)
    {
        auto* open = reinterpret_cast<MCI_OPEN_PARMSW*>(parameters);
        {
            std::scoped_lock lock(audioMutex);
            audioCurrent = open->lpstrElementName;
            audioOpened.push_back(audioCurrent);
        }
        if(stallAudio)
        {
            SetEvent(audioEntered);
            WaitForSingleObject(audioRelease, INFINITE);
        }
        if(failAudio) return MCIERR_FILE_NOT_FOUND;
        open->wDeviceID = 17;
    }
    else if(message == MCI_PLAY)
    {
        assert(flags == MCI_FROM);
        std::scoped_lock lock(audioMutex);
        audioPlayed.push_back(audioCurrent);
    }
    return 0;
}

BOOL TestWriteFile(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
    if(stallWrite)
    {
        SetEvent(ioEntered);
        WaitForSingleObject(ioRelease, INFINITE);
    }
    if(failWrite)
    {
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    return WriteFile(file, data, size, written, overlapped);
}

BOOL TestFlushFileBuffers(HANDLE file)
{
    if(stallFlush)
    {
        SetEvent(ioEntered);
        WaitForSingleObject(ioRelease, INFINITE);
    }
    return FlushFileBuffers(file);
}
}

#define WriteFile TestWriteFile
#define FlushFileBuffers TestFlushFileBuffers
#define mciSendCommandW TestMciSendCommandW
#include "../Client/pilot_telemetry.cpp"
#undef WriteFile
#undef FlushFileBuffers
#undef mciSendCommandW

namespace
{
struct TestLua
{
    std::string text;
    bool flush{true};
    bool result{};
    std::string error;
    std::string value;
    std::string lease;
};
std::map<std::string, DarkFlameLuaCFunction> callbacks;
}

const char* DarkFlameLuaToLString(void* lua, int index, size_t* size)
{
    const auto* state = static_cast<TestLua*>(lua);
    const auto& value = index == 3 ? state->lease : index == 2 ? state->value : state->text;
    *size = value.size();
    return value.c_str();
}

int DarkFlameLuaToBoolean(void* lua, int) { return static_cast<TestLua*>(lua)->flush; }
void DarkFlameLuaPushBoolean(void* lua, int value) { static_cast<TestLua*>(lua)->result = value != 0; }
void DarkFlameLuaPushString(void* lua, const char* value) { static_cast<TestLua*>(lua)->error = value; }
void DarkFlameLuaRegister(void*, const char* name, DarkFlameLuaCFunction fn) { callbacks[name] = fn; }

void WaitForDrain()
{
    const auto deadline = GetTickCount64() + 5000;
    for(;;)
    {
        {
            auto& writer = Writer();
            std::scoped_lock lock(writer.mutex);
            assert(writer.error.empty());
            if(writer.submitted == writer.flushed && writer.pendingBytes == 0) return;
        }
        assert(GetTickCount64() < deadline);
        Sleep(1);
    }
}

int main()
{
    const auto repo = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path().parent_path();
    const auto directory = repo / ".codex-temp-dia2dump" / "pilot-writer-test";
    std::filesystem::create_directories(directory);
    const auto path = directory / "PilotTelemetry.log";
    { std::ofstream old(path); old << "old game data\n"; }
    SetEnvironmentVariableW(BootstrapProtocol::LogDirectoryVariable, directory.c_str());
    InitializePilotTelemetry();
    const auto contents = [&]
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto header = contents();
    assert(header.find("game_start") != std::string::npos);
    assert(header.find("old game data") == std::string::npos);
    RegisterPilotTelemetryLua(nullptr);
    TestLua update;
    update.text = "attach";
    callbacks.at("dfPilotUpdate")(&update);
    update.lease = update.error;
    update.text = "loaded";
    update.value = "1";
    callbacks.at("dfPilotUpdate")(&update);
    update.text = "heartbeat";
    callbacks.at("dfPilotUpdate")(&update);
    update.text = "status";
    update.value = "Lua -> ImGui";
    callbacks.at("dfPilotUpdate")(&update);
    assert(g_state.at("status") == update.value);
    Queue("start");
    Queue("note:ImGui -> Lua");
    TestLua command;
    command.text = update.lease;
    assert(callbacks.at("dfPilotTakeCommand")(&command) == 1 && command.error == "start");
    assert(callbacks.at("dfPilotTakeCommand")(&command) == 1 && command.error == "note:ImGui -> Lua");
    assert(callbacks.at("dfPilotTakeCommand")(&command) == 0);
    Queue("must not survive a stale connection");
    g_heartbeat = GetTickCount64() - 3000;
    assert(callbacks.at("dfPilotTakeCommand")(&command) == 0);
    update.text = "heartbeat";
    callbacks.at("dfPilotUpdate")(&update);
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1536, 1024);
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels{};
    int width{}, height{};
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(1536, 1024));
    ImGui::Begin("Pilot test");
    DrawPilotTelemetry(ImVec2(100, 100), ImVec2(1326, 450), 1.0f);
    ImGui::End();
    ImGui::Render();
    assert(ImGui::GetDrawData()->TotalVtxCount > 0);
    ImGui::DestroyContext();
    Queue("must not survive reload");
    update.text = "loaded";
    update.value = "1";
    callbacks.at("dfPilotUpdate")(&update);
    assert(callbacks.at("dfPilotTakeCommand")(&command) == 0);
    TestLua attach;
    attach.text = "attach";
    callbacks.at("dfPilotUpdate")(&attach);
    update.text = "status";
    callbacks.at("dfPilotUpdate")(&update);
    assert(!update.result);
    update.lease = attach.error;
    callbacks.at("dfPilotUpdate")(&update);
    assert(update.result);
    TestLua lua{"{\"type\":\"test_first_flight\"}\n"};
    lua.lease = command.text;
    callbacks.at("dfPilotLog")(&lua);
    assert(!lua.result && lua.error == "collector_replaced");
    lua.lease = update.lease;
    callbacks.at("dfPilotLog")(&lua);
    assert(lua.result);
    WaitForDrain();
    InitializePilotTelemetry();
    assert(contents() == header + lua.text);
    const auto first = contents();
    lua.text = "{\"type\":\"test_second_flight\"}\n";
    callbacks.at("dfPilotLog")(&lua);
    WaitForDrain();
    assert(lua.result && contents() == first + lua.text);
    const auto second = contents();
    lua.text.assign(65537, 'x');
    callbacks.at("dfPilotLog")(&lua);
    assert(!lua.result && contents() == second);

    audioEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    audioRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stallAudio = true;
    update.text = "play_sound";
    update.value = "on";
    std::promise<void> audioReturned;
    auto audioFuture = audioReturned.get_future();
    std::thread audioProducer([&]
    {
        callbacks.at("dfPilotUpdate")(&update);
        audioReturned.set_value();
    });
    assert(WaitForSingleObject(audioEntered, 5000) == WAIT_OBJECT_0);
    assert(audioFuture.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    audioProducer.join();
    assert(update.result);
    // Supersede a slow ON open; only the most recent OFF may play afterwards.
    update.value = "off";
    callbacks.at("dfPilotUpdate")(&update);
    assert(update.result);
    {
        std::scoped_lock lock(Audio().mutex);
        assert(Audio().pending == 2);
    }
    update.value = "../arbitrary.mp3";
    callbacks.at("dfPilotUpdate")(&update);
    assert(!update.result);
    stallAudio = false;
    SetEvent(audioRelease);
    const auto audioDeadline = GetTickCount64() + 5000;
    for(;;)
    {
        bool played;
        {
            std::scoped_lock lock(audioMutex);
            played = !audioPlayed.empty();
            if(played)
            {
                assert(audioOpened.front() == (directory / "AutoPilotON.mp3").wstring());
                assert(audioPlayed.size() == 1);
                assert(audioPlayed.front() == (directory / "AirbusOff.mp3").wstring());
            }
        }
        if(played) break;
        assert(GetTickCount64() < audioDeadline);
        Sleep(1);
    }
    failAudio = true;
    update.value = "on";
    callbacks.at("dfPilotUpdate")(&update);
    const auto errorDeadline = GetTickCount64() + 5000;
    for(;;)
    {
        bool failed;
        {
            std::scoped_lock lock(g_mutex);
            failed = !g_state["autopilot_sound_error"].empty();
        }
        if(failed) break;
        assert(GetTickCount64() < errorDeadline);
        Sleep(1);
    }

    ioEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ioRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for(int pass = 0; pass < 2; ++pass)
    {
        ResetEvent(ioEntered);
        ResetEvent(ioRelease);
        stallWrite = pass == 0;
        stallFlush = pass == 1;
        lua.text = "{\"type\":\"delayed_io\"}\n";
        std::promise<void> returned;
        auto future = returned.get_future();
        std::thread producer([&]
        {
            callbacks.at("dfPilotLog")(&lua);
            returned.set_value();
        });
        assert(WaitForSingleObject(ioEntered, 5000) == WAIT_OBJECT_0);
        assert(future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
        producer.join();
        assert(lua.result);
        update.text = "heartbeat";
        callbacks.at("dfPilotUpdate")(&update);
        assert(update.result);
        {
            std::scoped_lock lock(Writer().mutex);
            assert(Writer().pendingBytes > 0 && Writer().submitted > Writer().flushed);
        }
        stallWrite = false;
        stallFlush = false;
        SetEvent(ioRelease);
        WaitForDrain();
    }

    // A periodic flush must finish a quiet tail without further Lua submissions.
    lua.text = "{\"type\":\"periodic_flush\"}\n";
    lua.flush = false;
    callbacks.at("dfPilotLog")(&lua);
    WaitForDrain();
    assert(contents().ends_with(lua.text));

    ResetEvent(ioEntered);
    ResetEvent(ioRelease);
    stallWrite = true;
    lua.text.assign(65536, 'x');
    lua.flush = true;
    callbacks.at("dfPilotLog")(&lua);
    assert(WaitForSingleObject(ioEntered, 5000) == WAIT_OBJECT_0);
    for(int i = 1; i < 64; ++i)
    {
        callbacks.at("dfPilotLog")(&lua);
        assert(lua.result);
    }
    callbacks.at("dfPilotLog")(&lua);
    assert(!lua.result && lua.error.find("queue full") != std::string::npos);
    {
        std::scoped_lock lock(Writer().mutex);
        assert(Writer().pendingBytes == 4 * 1024 * 1024);
    }
    stallWrite = false;
    SetEvent(ioRelease);
    {
        std::scoped_lock lock(Writer().mutex);
        Writer().error.clear();
    }
    WaitForDrain();

    failWrite = true;
    lua.text = "{\"type\":\"must_fail\"}\n";
    callbacks.at("dfPilotLog")(&lua);
    assert(lua.result);
    assert(WaitForSingleObject(Writer().thread, 5000) == WAIT_OBJECT_0);
    callbacks.at("dfPilotLog")(&lua);
    assert(!lua.result && !lua.error.empty());
    CloseHandle(Writer().thread);
    CloseHandle(Writer().wake);
    CloseHandle(ioEntered);
    CloseHandle(ioRelease);
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    std::puts("PASS: Lua/ImGui bridge, reset/append, stalled I/O and audio do not block callbacks, external MP3 paths, latest audio wins, bounded queue, asynchronous failures");
    return 0;
}
