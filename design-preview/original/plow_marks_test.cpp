#include <Windows.h>
#include <d3d9.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../Client/plow_bot.cpp"
#include "../Client/third_party/imgui/backends/imgui_impl_dx9.h"

namespace
{
struct TestLua
{
    std::vector<std::string> args;
    std::vector<std::string> pushed;
};
std::map<std::string, DarkFlameLuaCFunction> callbacks;

std::vector<std::string> Call(const char* name, std::vector<std::string> args)
{
    TestLua lua{std::move(args), {}};
    callbacks.at(name)(&lua);
    return lua.pushed;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::vector<std::string> TakeCommands()
{
    std::scoped_lock lock(g_mutex);
    std::vector<std::string> taken(g_commands.begin(), g_commands.end());
    g_commands.clear();
    return taken;
}

bool Contains(const std::vector<std::string>& list, const std::string& value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}
}

const char* DarkFlameLuaToLString(void* lua, int index, size_t* size)
{
    auto* state = static_cast<TestLua*>(lua);
    if(index < 1 || index > static_cast<int>(state->args.size()))
    {
        *size = 0;
        return nullptr;
    }
    const auto& value = state->args[index - 1];
    *size = value.size();
    return value.c_str();
}
int DarkFlameLuaToBoolean(void*, int) { return 0; }
void DarkFlameLuaPushBoolean(void* lua, int value)
{
    static_cast<TestLua*>(lua)->pushed.push_back(value ? "true" : "false");
}
void DarkFlameLuaPushString(void* lua, const char* value) { static_cast<TestLua*>(lua)->pushed.push_back(value); }
void DarkFlameLuaPushNil(void* lua) { static_cast<TestLua*>(lua)->pushed.push_back("<nil>"); }
void DarkFlameLuaRegister(void*, const char* name, DarkFlameLuaCFunction fn) { callbacks[name] = fn; }

namespace
{
IDirect3DDevice9* g_device{};

void Frame(ImGuiIO& io)
{
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    {
        std::scoped_lock lock(g_mutex);
        g_heartbeat = GetTickCount64();
    }
    DrawPlowBot(ImVec2(22, 20), ImVec2(1326, 860), 1);
    ImGui::End();
    ImGui::Render();
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 7, 6, 13), 1, 0);
    assert(SUCCEEDED(g_device->BeginScene()));
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    g_device->EndScene();
}

void Click(ImGuiIO& io, float x, float y)
{
    io.AddMousePosEvent(x, y);
    Frame(io);
    io.AddMouseButtonEvent(0, true);
    Frame(io);
    io.AddMouseButtonEvent(0, false);
    Frame(io);
}

void Capture(const std::filesystem::path& file)
{
    IDirect3DSurface9* target{};
    IDirect3DSurface9* staging{};
    assert(SUCCEEDED(g_device->GetRenderTarget(0, &target)));
    D3DSURFACE_DESC description{};
    target->GetDesc(&description);
    assert(SUCCEEDED(g_device->CreateOffscreenPlainSurface(description.Width, description.Height,
        description.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr)));
    assert(SUCCEEDED(g_device->GetRenderTargetData(target, staging)));
    D3DLOCKED_RECT locked{};
    assert(SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)));
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + description.Width * description.Height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = description.Width;
    info.biHeight = -static_cast<LONG>(description.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for(UINT y = 0; y < description.Height; ++y)
        output.write(static_cast<const char*>(locked.pBits) + y * locked.Pitch, description.Width * 4);
    staging->UnlockRect();
    staging->Release();
    target->Release();
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path out = argc > 1 ? std::filesystem::path(argv[1])
        : std::filesystem::temp_directory_path() / "plow_marks_test";
    std::filesystem::remove_all(out);
    std::filesystem::create_directories(out);
    SetEnvironmentVariableW(L"DARKFLAME_LOG_DIRECTORY", out.c_str());

    InitializePlowBot();
    RegisterPlowBotLua(nullptr);
    assert(callbacks.contains("dfPlowMarks"));
    const auto attach = Call("dfPlowUpdate", {"attach", ""});
    const std::string lease = attach.at(0);
    const auto marks = out / "PlowMarks.txt";
    const auto backup = out / "PlowMarks.bak";

    auto result = Call("dfPlowMarks", {"load", "", lease});
    assert(result.size() == 2 && result[0] == "<nil>" && result[1] == "missing");
    result = Call("dfPlowMarks", {"save", "x", "999"});
    assert(result[0] == "<nil>" && result[1] == "collector_replaced");
    assert(!std::filesystem::exists(marks));

    result = Call("dfPlowMarks", {"save", "{1.00, 2.00, 3.0, 4.00, nil},\n", lease});
    assert(result.size() == 2 && result[0] == "true" && result[1].empty());
    assert(ReadText(marks) == "{1.00, 2.00, 3.0, 4.00, nil},\n");
    assert(!std::filesystem::exists(backup));
    result = Call("dfPlowMarks", {"save", "second", lease});
    assert(result[0] == "true");
    result = Call("dfPlowMarks", {"load", "", lease});
    assert(result.size() == 1 && result[0] == "second");
    assert(!std::filesystem::exists(out / "PlowMarks.txt.tmp"));

    g_marksBackedUp = false;
    result = Call("dfPlowMarks", {"save", "third", lease});
    assert(result[0] == "true");
    assert(ReadText(backup) == "second" && ReadText(marks) == "third");

    WriteText(marks, "\xEF\xBB\xBFwith bom");
    result = Call("dfPlowMarks", {"load", "", lease});
    assert(result.size() == 1 && result[0] == "with bom");
    result = Call("dfPlowMarks", {"erase", "", lease});
    assert(result[0] == "<nil>" && result[1] == "bad_action");
    std::printf("marks file: OK\n");

    HWND window = CreateWindowW(L"STATIC", L"Plow UI preview", WS_POPUP, 0, 0, 1370, 900,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    assert(window && d3d);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferWidth = 1370;
    parameters.BackBufferHeight = 900;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    assert(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &g_device)));
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1370, 900);
    io.IniFilename = nullptr;
    io.FontDefault = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 20, nullptr,
        io.Fonts->GetGlyphRangesCyrillic());
    assert(io.FontDefault);
    ImGui::StyleColorsDark();
    ImGui_ImplDX9_Init(g_device);
    {
        std::scoped_lock lock(g_mutex);
        g_state = {{"loaded", "1"}, {"resource_ready", "1"}, {"bot", "0"}, {"editor", "0"},
            {"route", "Вокзал – Заводское шоссе"}, {"status", "Готов к запуску"},
            {"waypoints", "14"}, {"waypoint_cursor", "1"}, {"waypoint_last", "7"},
            {"waypoint_size", "2"}, {"traffic_info", "В памяти: 8."},
            {"dashboard", "0|1/54|100%|--|13 м|+120 с|0.00|0.00|--|461|0 мс|0"},
            {"wp_list", "1,2,13,1;2,2,45,1;3,4,70,1;4,5,2100,1;5,4,2180,1;6,2,2190,1;7,2,34,1;"
                "8,5,2170,1;9,5,120,1;10,2,250,1;11,2,160,1;12,2,80,1;13,10,60,1;14,14,40,1"},
            {"wp_edit", "7|9.61|2774.02|310.8|2|1|34|3"},
            {"wp_file", "Сохранено в PlowMarks.txt: 14 меток (#7 сдвинута)"},
            {"report", "00:10  Метки из PlowMarks.txt: 14\n00:10  PlowBot 2.7.0 подключён"}};
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_main.bmp");
    TakeCommands();

    assert(PlowScriptVersion("-- x\nlocal VERSION = \"2.11.2\"\n") == "2.11.2");
    assert(PlowScriptVersion("print(1)") == "?");
    assert(!PlowBotBusy());
    {
        std::scoped_lock lock(g_mutex);
        g_state["bot"] = "1";
    }
    assert(PlowBotBusy());
    {
        std::scoped_lock lock(g_mutex);
        g_state["bot"] = "0";
        g_state["autonomy"] = "1";
    }
    assert(PlowBotBusy());
    {
        std::scoped_lock lock(g_mutex);
        g_state["autonomy"] = "0";
    }
    PlowSetScript("2.11.2", 335000);
    PlowSetLoad(PlowLoad::Loaded, "Загружен автоматически в 17:15:20");
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_loader.bmp");
    assert(!PlowTakeReload());
    Click(io, 22 + 32 + 130, 20 + 500 * 1.22f + 24);
    assert(PlowTakeReload() && !PlowTakeReload());
    assert(PlowAutoReload());
    Click(io, 22 + 32 + 10, 20 + 462 * 1.22f + 12);
    assert(!PlowAutoReload());
    Click(io, 22 + 32 + 10, 20 + 462 * 1.22f + 12);
    assert(PlowAutoReload());
    assert(TakeCommands().empty());
    PlowSetLoad(PlowLoad::Pending, "Новая версия 2.11.3 загрузится, когда бот остановится");
    {
        std::scoped_lock lock(g_mutex);
        g_state["loaded"] = "0";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_loader_offline.bmp");
    PlowSetLoad(PlowLoad::Unloaded, "Выгружен во вкладке Lua Threads. «Загрузить скрипт» — вернуть");
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_loader_unloaded.bmp");
    Click(io, 22 + 32 + 130, 20 + 500 * 1.22f + 24);
    assert(PlowTakeReload());
    {
        std::scoped_lock lock(g_mutex);
        g_state["loaded"] = "1";
    }
    PlowSetLoad(PlowLoad::Loaded, "Загружен автоматически в 17:15:20");
    std::printf("loader card: OK\n");

    Click(io, 22 + 348 + 150, 20 + 518 * 1.22f + 15);
    for(int i = 0; i < 3; ++i) Frame(io);
    auto commands = TakeCommands();
    assert(Contains(commands, "editor:1"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["editor"] = "1";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_editor.bmp");

    Click(io, 22 + 32 + 120, 20 + 142 * 1.22f + 38 + 15);
    commands = TakeCommands();
    assert(Contains(commands, "wp_select:2"));
    Click(io, 22 + 412 + 220 - 64 + 138 + 64, 20 + 222 * 1.22f + 24);
    commands = TakeCommands();
    assert(Contains(commands, "wp_nudge:right:0.5"));
    Click(io, 22 + 412 + 64 + 3 * 92 + 10, 20 + 320 * 1.22f + 12);
    Click(io, 22 + 412 + 220, 20 + 176 * 1.22f + 24);
    commands = TakeCommands();
    assert(Contains(commands, "wp_nudge:fwd:2"));
    Click(io, 22 + 412 + 26 + 199 + 100, 20 + 380 * 1.22f + 24);
    Click(io, 22 + 412 + 16 + 200, 20 + 434 * 1.22f + 20);
    commands = TakeCommands();
    assert(Contains(commands, "wp_radius:+0.5") && Contains(commands, "wp_undo"));
    Click(io, 22 + 412 + 16 + 100, 20 + 478 * 1.22f + 20);
    Click(io, 22 + 412 + 26 + 199 + 100, 20 + 478 * 1.22f + 20);
    commands = TakeCommands();
    assert(Contains(commands, "wp_order:-1") && Contains(commands, "wp_order:1"));
    Capture(out / "plow_editor_order.bmp");
    Click(io, 22 + 1326 - 580 + 85, 20 + 12 * 1.22f + 18);
    Sleep(800);
    for(int i = 0; i < 3; ++i) Frame(io);
    commands = TakeCommands();
    assert(Contains(commands, "editor:0"));
    std::printf("editor page: OK\n");

    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();
    g_device->Release();
    d3d->Release();
    DestroyWindow(window);
    std::printf("ALL OK\n");
    return 0;
}
