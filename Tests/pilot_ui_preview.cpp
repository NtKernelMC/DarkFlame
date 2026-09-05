#define main PilotLogTestMain
#include "pilot_log_test.cpp"
#undef main
#include <d3d9.h>
#include "../Client/third_party/imgui/backends/imgui_impl_dx9.h"

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "active";
    const auto repo = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path().parent_path();
    const auto directory = repo / ".codex-temp-dia2dump" / "pilot-ui-preview";
    std::filesystem::create_directories(directory);
    HWND window = CreateWindowW(L"STATIC", L"Pilot UI preview", WS_POPUP, 0, 0, 1370, 650, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    assert(window && d3d);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferWidth = 1370;
    parameters.BackBufferHeight = 650;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device{};
    assert(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device)));
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1370, 650);
    io.IniFilename = nullptr;
    io.FontDefault = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 20, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    assert(io.FontDefault);
    ImGui::StyleColorsDark();
    ImGui_ImplDX9_Init(device);
    g_heartbeat = GetTickCount64();
    g_path = "C:\\DarkFlame\\PilotTelemetry.log";
    g_state = {{"loaded", "1"}, {"resource_ready", "1"}, {"autopilot", "1"}, {"recording", "1"},
        {"autopilot_telemetry", "1"}, {"autopilot_hud", "1"}, {"autopilot_status", "Рулёжка по маркерам"},
        {"destination", "Либерти Сити"}, {"interval_ms", "50"}, {"samples", "1248"}, {"notification_count", "12"},
        {"notification", "province:sendNotification | province_pilot\nДвигайтесь медленно по меткам для выруливания на взлетную полосу."},
        {"dashboard", "18|270|29|+0.0|8|+1.2|-0.4|-24.7|146|ВЫПУЩЕНЫ|ЗЕМЛЯ|checkpoint|20|14|0|+0"}};
    Writer().bytes = 3487752;
    if(mode == "standby")
    {
        g_state["autopilot"] = "0";
        g_state["recording"] = "0";
        g_state["autopilot_status"] = "Готов к запуску";
    }
    if(mode == "offline")
    {
        g_state.clear();
        g_heartbeat = 0;
    }
    if(mode == "fault") g_state["autopilot_hud_error"] = "dxDrawLine unavailable";
    for(int i = 0; i < 3; ++i)
    {
        ImGui_ImplDX9_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        DrawPilotTelemetry(ImVec2(22, 35), ImVec2(1326, 580), 1);
        ImGui::End();
        ImGui::Render();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 7, 6, 13), 1, 0);
        assert(SUCCEEDED(device->BeginScene()));
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
    IDirect3DSurface9* target{};
    IDirect3DSurface9* staging{};
    assert(SUCCEEDED(device->GetRenderTarget(0, &target)));
    D3DSURFACE_DESC description{};
    target->GetDesc(&description);
    assert(SUCCEEDED(device->CreateOffscreenPlainSurface(description.Width, description.Height,
        description.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr)));
    assert(SUCCEEDED(device->GetRenderTargetData(target, staging)));
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
    std::ofstream output(directory / (mode + ".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for(UINT y = 0; y < description.Height; ++y)
        output.write(static_cast<const char*>(locked.pBits) + y * locked.Pitch, description.Width * 4);
    staging->UnlockRect();
    staging->Release();
    target->Release();
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return 0;
}
