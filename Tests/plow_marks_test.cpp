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
#include "../Client/gui.cpp"
#include "../Client/third_party/imgui/imgui_internal.h"
#include "../Client/third_party/imgui/backends/imgui_impl_dx9.h"

void DrawPilotTelemetry(ImVec2, ImVec2 size, float) { ImGui::Dummy(size); }
void PulseLuaBridge() {}
namespace Log { void Write(std::wstring_view) {} }
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_RemoveHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_DisableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }

struct UiItem { ImRect rectangle, clip; bool disabled{}; };
std::map<ImGuiID, ImRect> uiRectangles;
std::map<std::string, UiItem> uiItems;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*)
{
    uiRectangles[id] = bb;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* context, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
    const char* suffix = std::strstr(label, "###");
    uiItems[suffix ? suffix + 3 : label] = {uiRectangles[id], context->CurrentWindow->ClipRect,
        (context->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0};
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return ""; }

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
    uiItems.clear();
    uiRectangles.clear();
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    {
        std::scoped_lock lock(g_mutex);
        g_heartbeat = GetTickCount64();
    }
    const int renderedTab = g_activeTab;
    RenderMenu();
    ImGui::Render();
    bool bannerDrawn = false;
    for(const auto* list : ImGui::GetDrawData()->CmdLists)
        for(const auto& command : list->CmdBuffer)
            if(command.ElemCount && !command.TexRef._TexData
                && command.GetTexID() == Texture(g_banner).GetTexID()) bannerDrawn = true;
    assert(bannerDrawn == (renderedTab != 6));
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

void ClickItem(ImGuiIO& io, const char* label)
{
    const auto item = uiItems.at(label);
    const auto center = item.rectangle.GetCenter();
    if(!item.clip.Contains(center)) std::fprintf(stderr, "Item outside visible pane: %s\n", label);
    assert(item.clip.Contains(center));
    Click(io, center.x, center.y);
    Frame(io);
    Frame(io);
}

void DragSlider(ImGuiIO& io, const char* label, const char* prefix)
{
    TakeCommands();
    const auto item = uiItems.at(label);
    const auto start = item.rectangle.Min;
    const float width = item.rectangle.GetWidth();
    const float y = item.rectangle.GetCenter().y;
    io.AddMousePosEvent(start.x + width * 0.25f, y);
    Frame(io);
    io.AddMouseButtonEvent(0, true);
    Frame(io);
    io.AddMousePosEvent(start.x + width * 0.85f, y);
    Frame(io);
    io.AddMouseButtonEvent(0, false);
    Frame(io);
    const auto commands = TakeCommands();
    assert(std::any_of(commands.begin(), commands.end(), [&](const std::string& value)
        { return value.rfind(prefix, 0) == 0; }));
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
    const UINT width = static_cast<UINT>(ImGui::GetIO().DisplaySize.x);
    const UINT height = static_cast<UINT>(ImGui::GetIO().DisplaySize.y);
    header.bfSize = header.bfOffBits + width * height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for(UINT y = 0; y < height; ++y)
        output.write(static_cast<const char*>(locked.pBits) + y * locked.Pitch, width * 4);
    staging->UnlockRect();
    staging->Release();
    target->Release();
}
}

int wmain(int argc, wchar_t** argv)
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

    const auto settings = out / "PlowBot.cfg";
    assert(g_autoRecord.load() && !std::filesystem::exists(settings));
    WriteText(settings, "AUTORECORD=0\n");
    LoadSettings();
    assert(!g_autoRecord.load());
    WriteText(settings, "AUTORECORD=1\n");
    LoadSettings();
    assert(g_autoRecord.load());
    std::filesystem::remove(settings);
    std::printf("settings file: OK\n");

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
    ConfigureFonts();
    ConfigureEditor();
    ImGui::StyleColorsDark();
    ConfigureStyle();
    GImGui->TestEngineHookItems = true;
    g_activeTab = 6;
    g_module = GetModuleHandleW(nullptr);
    assert(LoadTexture(g_device, IDR_DARK_FLAME_BACKGROUND, &g_background));
    assert(LoadTexture(g_device, IDR_DARK_FLAME_BANNER, &g_banner));
    ImGui_ImplDX9_Init(g_device);
    {
        std::scoped_lock lock(g_mutex);
        g_state = {{"loaded", "1"}, {"resource_ready", "1"}, {"bot", "0"}, {"editor", "0"},
            {"signals", "1"}, {"dtp_stop", "1"}, {"autorecord", "1"},
            {"route", "Вокзал – Заводское шоссе"}, {"status", "Готов к запуску"},
            {"waypoints", "14"}, {"waypoint_cursor", "1"}, {"waypoint_last", "7"},
            {"waypoint_size", "2"}, {"traffic_info", "В памяти: 8."},
            {"dashboard", "0|1/54|100%|--|13 м|+120 с|0.00|0.00|--|461|0 мс|0"},
            {"wp_list", "1,2,13,1;2,2,45,1;3,4,70,1;4,5,2100,1;5,4,2180,1;6,2,2190,1;7,2,34,1;"
                "8,5,2170,1;9,5,120,1;10,2,250,1;11,2,160,1;12,2,80,1;13,10,60,1;14,14,40,1"},
            {"wp_edit", "7|9.61|2774.02|310.8|2|1|34|3|#7"},
            {"ed_cities", "Приволжск|Мирный|Невский"}, {"ed_city", "1"},
            {"ed_routes", "1:Вокзал – Заводское шоссе:17;2:Вокзал – Набережная:0;3:Стадион – Троллейбусное депо:0"},
            {"ed_route", "0"}, {"ed_points", ""},
            {"ed_lights", "0,51,95;3,90,462;3,90,833;3,90,1204;3,90,1525;3,88,2076;3,270,2322;3,270,2613"},
            {"wp_file", "Сохранено в PlowMarks.txt: 14 меток (#7 сдвинута)"},
            {"report", "00:10  Метки из PlowMarks.txt: 14\n00:10  PlowBot 2.7.0 подключён"}};
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_main.bmp");
    TakeCommands();
    ClickItem(io, "plow_toggle");
    assert(Contains(TakeCommands(), "bot_start"));
    ClickItem(io, "plow_record");
    assert(Contains(TakeCommands(), "record_start"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["recording"] = "1";
    }
    Frame(io);
    ClickItem(io, "plow_record");
    assert(Contains(TakeCommands(), "record_stop"));

    ClickItem(io, "plow_autorecord");
    assert(!g_autoRecord.load() && ReadText(settings) == "AUTORECORD=0\n");
    assert(Contains(TakeCommands(), "autorecord:0"));
    Capture(out / "plow_autorecord_off.bmp");
    ClickItem(io, "plow_autorecord");
    assert(g_autoRecord.load() && ReadText(settings) == "AUTORECORD=1\n");
    TakeCommands();

    ClickItem(io, "plow_signals");
    assert(Contains(TakeCommands(), "signals:0"));
    ClickItem(io, "plow_dtp");
    assert(Contains(TakeCommands(), "dtp_stop:0"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["dtp_hold"] = "1";
        g_state["status"] = "ДТП: столкновение с машиной. Стою на аварийке — разберись и запусти бота";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_crash_hold.bmp");
    assert(PlowBotBusy());
    ClickItem(io, "plow_toggle");
    assert(Contains(TakeCommands(), "bot_start"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["dtp_hold"] = "0";
        g_state["status"] = "Готов к запуску";
    }
    std::printf("signals and crash stop: OK\n");

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
    ClickItem(io, "plow_reload");
    assert(PlowTakeReload() && !PlowTakeReload());
    assert(PlowAutoReload());
    ClickItem(io, "plow_auto_reload");
    assert(!PlowAutoReload());
    ClickItem(io, "plow_auto_reload");
    assert(PlowAutoReload());
    assert(TakeCommands().empty());
    PlowSetLoad(PlowLoad::Pending, "Новая версия 2.11.3 загрузится, когда бот остановится");
    {
        std::scoped_lock lock(g_mutex);
        g_state["loaded"] = "0";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_loader_offline.bmp");
    assert(uiItems.at("plow_toggle").disabled && uiItems.at("plow_record").disabled);
    assert(uiItems.at("##plow_limit").disabled);
    ClickItem(io, "plow_toggle");
    ClickItem(io, "plow_record");
    assert(TakeCommands().empty());
    PlowSetLoad(PlowLoad::Unloaded, "Выгружен во вкладке Lua Threads. «Загрузить скрипт» — вернуть");
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_loader_unloaded.bmp");
    ClickItem(io, "plow_reload");
    assert(PlowTakeReload());
    {
        std::scoped_lock lock(g_mutex);
        g_state["loaded"] = "1";
    }
    PlowSetLoad(PlowLoad::Loaded, "Загружен автоматически в 17:15:20");
    std::printf("loader card: OK\n");

    ClickItem(io, "Редактор меток");
    for(int i = 0; i < 3; ++i) Frame(io);
    auto commands = TakeCommands();
    assert(Contains(commands, "editor:1"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["editor"] = "1";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_editor.bmp");

    ClickItem(io, "#2  зона 2 м  /  45 м");
    commands = TakeCommands();
    assert(Contains(commands, "wp_select:2"));
    ClickItem(io, "plow_ed_right");
    commands = TakeCommands();
    assert(Contains(commands, "wp_nudge:right:0.5"));
    ClickItem(io, "plow_ed_step3");
    ClickItem(io, "plow_ed_fwd");
    commands = TakeCommands();
    assert(Contains(commands, "wp_nudge:fwd:2"));
    ClickItem(io, "plow_ed_bigger");
    ClickItem(io, "plow_ed_undo");
    commands = TakeCommands();
    assert(Contains(commands, "wp_radius:+0.5") && Contains(commands, "wp_undo"));
    ClickItem(io, "plow_ed_earlier");
    ClickItem(io, "plow_ed_later");
    commands = TakeCommands();
    assert(Contains(commands, "wp_order:-1") && Contains(commands, "wp_order:1"));
    Capture(out / "plow_editor_order.bmp");

    // Города: переключение, точки города, «Создать маршрут».
    ClickItem(io, "plow_ed_city1");
    commands = TakeCommands();
    assert(Contains(commands, "ed_city:2"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["ed_city"] = "2";
        g_state["ed_routes"] = "4:Проспект Мира – Западный берег:0;5:Мирный-Сити – Северный мост:6;"
            "6:Мирный-Сити – Восточный берег:0";
        g_state["ed_points"] = "gate,3,72;finish,4.5,45";
        g_state["ed_lights"] = "";
        g_state["wp_edit"] = "0|15.90|551.19|358.9|4.5|-|45|1|finish";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_editor_city.bmp");
    assert(uiItems.at("plow_ed_earlier").disabled && uiItems.at("plow_ed_later").disabled);
    assert(!uiItems.at("plow_ed_left").disabled);
    ClickItem(io, "Ворота депо  зона 3 м  /  72 м");
    ClickItem(io, "plow_city_gate");
    commands = TakeCommands();
    assert(Contains(commands, "wp_select:gate") && Contains(commands, "city_point:gate"));
    ClickItem(io, "plow_ed_new");
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_route_new.bmp");
    ClickItem(io, "plow_new_r5");
    commands = TakeCommands();
    assert(Contains(commands, "route_new:2:5"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["ed_work"] = "5";
        g_state["ed_route"] = "5";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_editor_work.bmp");
    ClickItem(io, "plow_ed_done");
    commands = TakeCommands();
    assert(Contains(commands, "route_done"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["ed_work"] = "";
        g_state["ed_route"] = "0";
        g_state["wp_edit"] = "7|9.61|2774.02|310.8|2|1|34|3|#7";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    std::printf("editor cities: OK\n");
    assert(!uiItems.contains("plow_ed_bind"));
    {
        std::scoped_lock lock(g_mutex);
        const auto at = g_state["wp_list"].find("3,4,70,1");
        g_state["wp_list"].replace(at, 8, "3,4,70,-");
        g_state["wp_edit"] = "3|2212.06|2488.88|221.2|4|-|70|0";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    Capture(out / "plow_editor_noroute.bmp");
    assert(uiItems.contains("! #3  зона 4 м  /  70 м"));
    ClickItem(io, "plow_ed_bind");
    assert(Contains(TakeCommands(), "wp_route:bind"));
    {
        std::scoped_lock lock(g_mutex);
        g_state["wp_list"].replace(g_state["wp_list"].find("3,4,70,-"), 8, "3,4,70,1");
        g_state["wp_edit"] = "7|9.61|2774.02|310.8|2|1|34|3";
    }
    for(int i = 0; i < 3; ++i) Frame(io);
    assert(!uiItems.contains("plow_ed_bind"));
    ClickItem(io, "Обзор");
    Sleep(800);
    for(int i = 0; i < 3; ++i) Frame(io);
    commands = TakeCommands();
    assert(Contains(commands, "editor:0"));
    std::printf("editor page: OK\n");

    {
        std::scoped_lock lock(g_mutex);
        g_state["editor"] = "0";
        g_state["traffic_working"] = "1";
        g_state["traffic_state"] = "3";
        g_state["traffic_info"] = "В памяти: 8. Сохраняй, стоя на стоп-линии, когда горит зелёный.";
    }
    ClickItem(io, "Светофоры и метки");
    assert(!uiItems.contains("plow_open_editor"));
    DragSlider(io, "##plow_limit", "speed_limit:");
    DragSlider(io, "##plow_wp_size", "waypoint_size:");
    std::printf("shared speed and waypoint sliders: OK\n");
    Capture(out / "plow_lights.bmp");
    ClickItem(io, "plow_light_test");
    assert(Contains(TakeCommands(), "test_traffic"));
    ClickItem(io, "plow_light_save");
    assert(Contains(TakeCommands(), "save_traffic"));
    ClickItem(io, "plow_light_forget");
    assert(Contains(TakeCommands(), "forget_traffic"));
    ClickItem(io, "Светофоры и метки");
    Capture(out / "plow_marks.bmp");
    ClickItem(io, "plow_wp_small");
    assert(Contains(TakeCommands(), "save_waypoint:small"));
    ClickItem(io, "plow_wp_mid");
    assert(Contains(TakeCommands(), "save_waypoint:mid"));
    ClickItem(io, "plow_wp_big");
    assert(Contains(TakeCommands(), "save_waypoint:big"));
    ClickItem(io, "plow_wp_forget");
    assert(Contains(TakeCommands(), "forget_waypoint"));

    for(const ImVec2 display : {ImVec2(1200, 740), ImVec2(1280, 720), ImVec2(1024, 768)})
    {
        io.DisplaySize = display;
        Frame(io);
        ClickItem(io, "Обзор");
        for(int i = 0; i < 3; ++i) Frame(io);
        const auto recordRect = uiItems.at("plow_record").rectangle;
        const auto navRect = uiItems.at("Обзор").rectangle;
        for(auto* pane : GImGui->Windows)
        {
            const char* suffix = std::strrchr(pane->Name, '/');
            suffix = suffix ? suffix + 1 : pane->Name;
            if(std::strncmp(suffix, "##plow_header_", 14) == 0 || std::strncmp(suffix, "##plow_bot_", 11) == 0)
            {
                if(pane->ScrollMax.y != 0) std::fprintf(stderr, "Unexpected parent scroll: %s / %.1f\n", suffix, pane->ScrollMax.y);
                assert(pane->ScrollMax.y == 0);
            }
        }
        const std::string dimensions = std::to_string(static_cast<int>(display.x)) + "x"
            + std::to_string(static_cast<int>(display.y));
        Capture(out / ("overview_" + dimensions + ".bmp"));
        ClickItem(io, "Светофоры и метки");
        Capture(out / ("lights_" + dimensions + ".bmp"));
        assert(uiItems.at("plow_wp_forget").clip.Contains(uiItems.at("plow_wp_forget").rectangle.GetCenter()));
        ClickItem(io, "Светофоры и метки");
        Capture(out / ("marks_" + dimensions + ".bmp"));
        ClickItem(io, "Редактор меток");
        Capture(out / ("editor_" + dimensions + ".bmp"));
        // Scrolling the editor must keep recording and page navigation in place.
        const auto center = uiItems.at("plow_ed_undo").rectangle.GetCenter();
        io.AddMousePosEvent(center.x, std::min(center.y, display.y - 80));
        io.AddMouseWheelEvent(0, -8);
        for(int i = 0; i < 3; ++i) Frame(io);
        assert(uiItems.at("plow_record").rectangle.Min.y == recordRect.Min.y);
        assert(uiItems.at("Обзор").rectangle.Min.y == navRect.Min.y);
    }
    std::printf("commands, offline controls and viewport layouts: OK\n");

    io.DisplaySize = ImVec2(1200, 740);
    {
        std::scoped_lock lock(g_mutex);
        g_state["route"] = "Вокзал — Заводское шоссе — длинное название маршрута";
        g_state["status"] = "ЗАПИСЬ: 40421 сэмплов, точек 199 — Рейс закрыт: маршрут сброшен сервером — "
            "метки и путь сброшены до нового маршрута";
        g_state["dashboard"] = "50|199/199|100%|КРАСНЫЙ|9999 м|-120 с|0.00|0.00|препятствие впереди|9999|нет|123";
        g_state["loaded"] = "0";
    }
    Frame(io);
    ClickItem(io, "Обзор");
    Capture(out / "offline_1200x740.bmp");
    {
        std::scoped_lock lock(g_mutex);
        g_state["loaded"] = "1";
    }
    Frame(io);
    Frame(io);
    Capture(out / "long_status_1200x740.bmp");
    for(int tab = 0; tab < 6; ++tab)
    {
        g_activeTab = tab;
        Frame(io);
        Frame(io);
    }
    std::printf("banner hidden only on Plow tab: OK\n");

    g_background->Release();
    g_banner->Release();
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();
    g_device->Release();
    d3d->Release();
    DestroyWindow(window);
    std::printf("ALL OK\n");
    return 0;
}
