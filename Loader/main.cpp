#include "config.h"
#include "launcher.h"
#include "path_utils.h"
#include "loader_resources.h"
#include "../Shared/runtime_log.h"

#include <Windows.h>
#include <d3d9.h>
#include <windowsx.h>
#include <wincodec.h>

#include "../Client/third_party/imgui/imgui.h"
#include "../Client/third_party/imgui/backends/imgui_impl_dx9.h"
#include "../Client/third_party/imgui/backends/imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window,
    UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr int WindowWidth = 1152;
constexpr int WindowHeight = 768;
constexpr std::size_t MaxMemoBytes = 256 * 1024;

enum class LaunchState
{
    Idle,
    Running,
    Success,
    Failed
};

HWND g_window{};
IDirect3D9* g_d3d{};
IDirect3DDevice9* g_device{};
D3DPRESENT_PARAMETERS g_parameters{};
IDirect3DTexture9* g_background{};
IDirect3DTexture9* g_banner{};
ImFont* g_titleFont{};
ImFont* g_regularFont{};
ImFont* g_buttonFont{};
ImFont* g_codeFont{};
ImFont* g_serialFont{};
std::wstring g_directory;
DarkFlameConfig g_config;
std::array<char, 33> g_serial{};
std::mutex g_logMutex;
std::string g_logText;
std::vector<char> g_memo{1, '\0'};
std::atomic_bool g_logDirty{};
std::atomic<LaunchState> g_launchState{LaunchState::Idle};
std::jthread g_worker;

ImTextureRef Texture(IDirect3DTexture9* texture)
{
    return ImTextureRef(reinterpret_cast<void*>(texture));
}

std::string Utf8(std::wstring_view value)
{
    if(value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if(size <= 0)
        return {};
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        output.data(), size, nullptr, nullptr);
    return output;
}

void AppendLog(std::wstring_view text)
{
    RuntimeLog::Write(text);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[24]{};
    swprintf_s(timestamp, L"[%02u:%02u:%02u] ",
        now.wHour, now.wMinute, now.wSecond);
    std::string line = Utf8(timestamp);
    line += Utf8(text);
    line.push_back('\n');
    std::scoped_lock lock(g_logMutex);
    g_logText += line;
    if(g_logText.size() > MaxMemoBytes)
    {
        const std::size_t cut = g_logText.size() - MaxMemoBytes;
        const std::size_t newline = g_logText.find('\n', cut);
        g_logText.erase(0, newline == std::string::npos ? cut : newline + 1);
    }
    g_logDirty.store(true, std::memory_order_release);
}

void SyncMemo()
{
    if(!g_logDirty.exchange(false, std::memory_order_acq_rel))
        return;
    std::scoped_lock lock(g_logMutex);
    g_memo.assign(g_logText.begin(), g_logText.end());
    g_memo.push_back('\0');
}

bool SaveConfig(bool reportError = true)
{
    const std::string serial(g_serial.data());
    if(Config::ValidSerial(serial))
    {
        g_config.publicSerial = serial;
        g_config.serialValid = true;
    }
    const bool saved = Config::Save(g_directory, g_config);
    if(!saved && reportError)
        AppendLog(L"[error] DarkFlame.cfg could not be saved");
    return saved;
}

int FilterSerial(ImGuiInputTextCallbackData* data)
{
    const ImWchar ch = data->EventChar;
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z') ? 0 : 1;
}

bool LoadTexture(int resourceId, IDirect3DTexture9** output)
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const DWORD resourceSize = resource ? SizeofResource(module, resource) : 0;
    auto* bytes = loaded ? static_cast<BYTE*>(LockResource(loaded)) : nullptr;
    if(!bytes || !resourceSize)
        return false;

    IWICImagingFactory* factory{};
    IWICStream* stream{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    IDirect3DTexture9* texture{};
    bool success{};
    if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromMemory(bytes, resourceSize))
        && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder))
        && SUCCEEDED(decoder->GetFrame(0, &frame))
        && SUCCEEDED(factory->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    {
        UINT width{}, height{};
        if(SUCCEEDED(converter->GetSize(&width, &height))
            && SUCCEEDED(g_device->CreateTexture(width, height, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
        {
            D3DLOCKED_RECT locked{};
            if(SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0)))
            {
                success = SUCCEEDED(converter->CopyPixels(nullptr,
                    static_cast<UINT>(locked.Pitch),
                    static_cast<UINT>(locked.Pitch) * height,
                    static_cast<BYTE*>(locked.pBits)));
                texture->UnlockRect(0);
            }
        }
    }
    if(converter) converter->Release();
    if(frame) frame->Release();
    if(decoder) decoder->Release();
    if(stream) stream->Release();
    if(factory) factory->Release();
    if(!success && texture)
    {
        texture->Release();
        texture = nullptr;
    }
    *output = texture;
    return success;
}

bool ApplyWindowShape(HWND window, IDirect3DTexture9* texture)
{
    D3DSURFACE_DESC description{};
    D3DLOCKED_RECT locked{};
    if(!window || !texture || FAILED(texture->GetLevelDesc(0, &description))
        || description.Format != D3DFMT_A8R8G8B8
        || FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)))
    {
        return false;
    }

    std::vector<RECT> rectangles;
    rectangles.reserve(WindowHeight * 2);
    constexpr BYTE AlphaThreshold = 20;
    for(int y{}; y < WindowHeight; ++y)
    {
        const UINT sourceY = static_cast<UINT>(y) * description.Height / WindowHeight;
        const auto* row = static_cast<const BYTE*>(locked.pBits)
            + static_cast<std::size_t>(sourceY) * locked.Pitch;
        for(int x{}; x < WindowWidth;)
        {
            const auto alpha = [&](int position)
            {
                const UINT sourceX = static_cast<UINT>(position)
                    * description.Width / WindowWidth;
                return row[sourceX * 4 + 3];
            };
            while(x < WindowWidth && alpha(x) <= AlphaThreshold)
                ++x;
            const int left = x;
            while(x < WindowWidth && alpha(x) > AlphaThreshold)
                ++x;
            if(left < x)
                rectangles.push_back({left, y, x, y + 1});
        }
    }
    texture->UnlockRect(0);
    if(rectangles.empty())
        return false;

    const std::size_t dataSize = sizeof(RGNDATAHEADER)
        + rectangles.size() * sizeof(RECT);
    std::vector<BYTE> bytes(dataSize);
    auto* header = reinterpret_cast<RGNDATAHEADER*>(bytes.data());
    header->dwSize = sizeof(RGNDATAHEADER);
    header->iType = RDH_RECTANGLES;
    header->nCount = static_cast<DWORD>(rectangles.size());
    header->nRgnSize = static_cast<DWORD>(rectangles.size() * sizeof(RECT));
    header->rcBound = {0, 0, WindowWidth, WindowHeight};
    std::memcpy(bytes.data() + sizeof(RGNDATAHEADER), rectangles.data(),
        rectangles.size() * sizeof(RECT));
    HRGN region = ExtCreateRegion(nullptr, static_cast<DWORD>(bytes.size()),
        reinterpret_cast<RGNDATA*>(bytes.data()));
    if(!region)
        return false;
    if(SetWindowRgn(window, region, FALSE))
        return true;
    DeleteObject(region);
    return false;
}

void ConfigureStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.ScrollbarSize = 12.0f;
    style.FramePadding = {10.0f, 7.0f};
    style.ItemSpacing = {10.0f, 8.0f};
    style.Colors[ImGuiCol_Text] = ImVec4(0.89f, 0.85f, 0.97f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.39f, 0.54f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.025f, 0.012f, 0.055f, 0.98f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.04f, 0.24f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.06f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.89f, 0.34f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.012f, 0.006f, 0.03f, 0.95f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.48f, 0.10f, 0.75f, 0.80f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.01f, 0.04f, 0.9f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.32f, 0.08f, 0.52f, 0.9f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.58f, 0.12f, 0.88f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.75f, 0.22f, 1.0f, 1.0f);
}

void ConfigureFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFont* fallback = io.Fonts->AddFontDefault();
    io.FontDefault = fallback;
    const ImWchar* glyphs = io.Fonts->GetGlyphRangesCyrillic();
    g_titleFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeuib.ttf", 23.0f, nullptr, glyphs);
    g_regularFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf", 19.0f, nullptr, glyphs);
    g_buttonFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeuib.ttf", 30.0f, nullptr, glyphs);
    g_codeFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\consola.ttf", 16.0f, nullptr, glyphs);
    g_serialFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\consolab.ttf", 21.0f, nullptr, glyphs);
    if(!g_titleFont) g_titleFont = fallback;
    if(!g_regularFont) g_regularFont = fallback;
    if(!g_buttonFont) g_buttonFont = fallback;
    if(!g_codeFont) g_codeFont = fallback;
    if(!g_serialFont) g_serialFont = g_codeFont;
}

void DrawFlame(ImDrawList* draw, ImVec2 center, float size)
{
    draw->AddCircleFilled(center, size * 0.62f, IM_COL32(123, 24, 220, 110), 24);
    const ImVec2 outer[] = {
        {center.x, center.y - size},
        {center.x + size * 0.62f, center.y - size * 0.08f},
        {center.x + size * 0.35f, center.y + size * 0.7f},
        {center.x, center.y + size},
        {center.x - size * 0.48f, center.y + size * 0.45f},
        {center.x - size * 0.6f, center.y - size * 0.16f}};
    draw->AddConvexPolyFilled(outer, 6, IM_COL32(204, 65, 255, 255));
    const ImVec2 inner[] = {
        {center.x + size * 0.08f, center.y - size * 0.46f},
        {center.x + size * 0.3f, center.y + size * 0.12f},
        {center.x, center.y + size * 0.55f},
        {center.x - size * 0.22f, center.y + size * 0.08f}};
    draw->AddConvexPolyFilled(inner, 4, IM_COL32(249, 218, 255, 255));
}

void DrawPanel(ImDrawList* draw, ImVec2 topLeft, ImVec2 size)
{
    draw->AddRectFilled(topLeft, {topLeft.x + size.x, topLeft.y + size.y},
        IM_COL32(4, 2, 14, 244), 9.0f);
    draw->AddRect(topLeft, {topLeft.x + size.x, topLeft.y + size.y},
        IM_COL32(163, 43, 226, 220), 9.0f, 0, 2.0f);
    draw->AddRect({topLeft.x + 5.0f, topLeft.y + 5.0f},
        {topLeft.x + size.x - 5.0f, topLeft.y + size.y - 5.0f},
        IM_COL32(75, 18, 111, 150), 6.0f);
}

bool DrawActionButton(const char* label, ImVec2 position, ImVec2 size,
    bool enabled, ImDrawList* draw)
{
    ImGui::SetCursorScreenPos(position);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##launch", size);
    const bool hovered = enabled && ImGui::IsItemHovered();
    ImGui::EndDisabled();
    const ImU32 border = enabled
        ? hovered ? IM_COL32(242, 124, 255, 255) : IM_COL32(192, 67, 245, 245)
        : IM_COL32(83, 55, 96, 190);
    draw->AddRectFilled(position, {position.x + size.x, position.y + size.y},
        enabled ? IM_COL32(13, 4, 27, 245) : IM_COL32(11, 8, 16, 235), 13.0f);
    draw->AddRect(position, {position.x + size.x, position.y + size.y}, border,
        13.0f, 0, 3.0f);
    const float fontSize = g_buttonFont->LegacySize;
    const ImVec2 textSize = g_buttonFont->CalcTextSizeA(fontSize,
        FLT_MAX, 0.0f, label);
    draw->AddText(g_buttonFont, fontSize,
        {position.x + (size.x - textSize.x) * 0.5f,
        position.y + (size.y - textSize.y) * 0.45f},
        enabled ? IM_COL32(252, 238, 255, 255) : IM_COL32(112, 99, 122, 255),
        label);
    for(int side : {-1, 1})
    {
        const float base = side < 0 ? position.x + 55.0f
            : position.x + size.x - 55.0f;
        for(int index{}; index < 3; ++index)
        {
            const float offset = index * 11.0f * side;
            draw->AddLine({base + offset - 7.0f * side, position.y + size.y * 0.37f},
                {base + offset, position.y + size.y * 0.5f}, border, 2.5f);
            draw->AddLine({base + offset, position.y + size.y * 0.5f},
                {base + offset - 7.0f * side, position.y + size.y * 0.63f},
                border, 2.5f);
        }
    }
    return clicked && enabled;
}

void StartLaunch()
{
    if(g_launchState.load(std::memory_order_acquire) == LaunchState::Running)
        return;
    const std::string serial(g_serial.data());
    if(g_config.setSerial && !Config::ValidSerial(serial))
    {
        AppendLog(L"[error] Black Mirror requires a 32-character serial");
        return;
    }
    if(g_worker.joinable())
        g_worker.join();
    SaveConfig();
    const DarkFlameConfig config = g_config;
    g_launchState.store(LaunchState::Running, std::memory_order_release);
    AppendLog(L"[loader] launch requested");
    g_worker = std::jthread([config](std::stop_token stop)
    {
        const int result = Launcher::Run(config, stop, [](std::wstring_view line)
        {
            AppendLog(line);
        });
        if(stop.stop_requested())
            return;
        if(result != 0)
        {
            g_launchState.store(LaunchState::Failed, std::memory_order_release);
            return;
        }
        g_launchState.store(LaunchState::Success, std::memory_order_release);
        AppendLog(L"[loader] loaded successfully; closing in 3 seconds...");
        for(int step{}; step < 30 && !stop.stop_requested(); ++step)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(!stop.stop_requested())
            PostMessageW(g_window, WM_CLOSE, 0, 0);
    });
}

void DrawSettings(ImVec2 position, ImVec2 size)
{
    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 17.0f});
    ImGui::PushFont(g_titleFont, g_titleFont->LegacySize);
    ImGui::TextColored(ImVec4(0.88f, 0.48f, 1.0f, 1.0f), "Loader Settings");
    ImGui::PopFont();

    ImGui::PushFont(g_regularFont, g_regularFont->LegacySize);
    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 62.0f});
    if(ImGui::Checkbox("Anti-Shadow", &g_config.antiShadow))
        SaveConfig();

    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 112.0f});
    if(ImGui::Checkbox("Black Mirror", &g_config.setSerial))
    {
        if(g_config.setSerial)
            g_config.randomSerial = false;
        SaveConfig();
    }

    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 162.0f});
    if(ImGui::Checkbox("Random Serial", &g_config.randomSerial))
    {
        if(g_config.randomSerial)
            g_config.setSerial = false;
        SaveConfig();
    }

    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 219.0f});
    ImGui::TextUnformatted("Serial");
    ImGui::PopFont();

    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 245.0f});
    ImGui::SetNextItemWidth(size.x - 40.0f);
    ImGui::PushFont(g_serialFont, g_serialFont->LegacySize);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0f, 9.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(7, 2, 18, 252));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(180, 52, 239, 235));
    const bool changed = ImGui::InputText("##serial", g_serial.data(), g_serial.size(),
        ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CallbackCharFilter,
        &FilterSerial);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if(changed && Config::ValidSerial(g_serial.data()))
        SaveConfig();

    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 295.0f});
    ImGui::PushFont(g_codeFont, g_codeFont->LegacySize);
    if(g_config.setSerial && !Config::ValidSerial(g_serial.data()))
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.48f, 1.0f),
            "Serial must contain exactly 32 characters.");
    else
        ImGui::TextDisabled("32 uppercase letters or digits.");
    ImGui::PopFont();
}

void DrawMemo(ImVec2 position, ImVec2 size)
{
    SyncMemo();
    ImGui::SetCursorScreenPos({position.x + 20.0f, position.y + 17.0f});
    ImGui::PushFont(g_titleFont, g_titleFont->LegacySize);
    ImGui::TextColored(ImVec4(0.88f, 0.48f, 1.0f, 1.0f), "Loader Output");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({position.x + 17.0f, position.y + 55.0f});
    ImGui::PushFont(g_codeFont, g_codeFont->LegacySize);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(2, 1, 9, 242));
    ImGui::InputTextMultiline("##loader_log", g_memo.data(), g_memo.size(),
        {size.x - 34.0f, size.y - 72.0f},
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void DrawWindow()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##dark_flame_loader", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddImage(Texture(g_background), origin,
        {origin.x + display.x, origin.y + display.y});
    draw->AddImage(Texture(g_banner), {origin.x + 61.0f, origin.y + 84.0f},
        {origin.x + 1091.0f, origin.y + 304.0f});
    DrawFlame(draw, {origin.x + 72.0f, origin.y + 46.0f}, 15.0f);

    const char* title = "Dark Flame Loader by DroidZero";
    const float titleSize = g_titleFont->LegacySize;
    const ImVec2 textSize = g_titleFont->CalcTextSizeA(titleSize,
        FLT_MAX, 0.0f, title);
    draw->AddText(g_titleFont, titleSize,
        {origin.x + (display.x - textSize.x) * 0.5f, origin.y + 31.0f},
        IM_COL32(246, 231, 255, 255), title);

    const ImVec2 closePosition{origin.x + display.x - 104.0f, origin.y + 24.0f};
    const ImVec2 closeSize{42.0f, 42.0f};
    ImGui::SetCursorScreenPos(closePosition);
    if(ImGui::InvisibleButton("##close", closeSize))
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    const bool closeHovered = ImGui::IsItemHovered();
    const ImU32 closeColor = closeHovered ? IM_COL32(245, 126, 255, 255)
        : IM_COL32(220, 82, 255, 255);
    draw->AddRectFilled(closePosition,
        {closePosition.x + closeSize.x, closePosition.y + closeSize.y},
        closeHovered ? IM_COL32(54, 9, 76, 245) : IM_COL32(8, 3, 19, 238), 8.0f);
    draw->AddRect(closePosition,
        {closePosition.x + closeSize.x, closePosition.y + closeSize.y},
        IM_COL32(35, 5, 48, 255), 8.0f, 0, 6.0f);
    draw->AddRect({closePosition.x + 2.0f, closePosition.y + 2.0f},
        {closePosition.x + closeSize.x - 2.0f, closePosition.y + closeSize.y - 2.0f},
        closeColor, 7.0f, 0, 2.0f);
    const ImVec2 crossA{closePosition.x + 13.0f, closePosition.y + 13.0f};
    const ImVec2 crossB{closePosition.x + 29.0f, closePosition.y + 29.0f};
    const ImVec2 crossC{closePosition.x + 29.0f, closePosition.y + 13.0f};
    const ImVec2 crossD{closePosition.x + 13.0f, closePosition.y + 29.0f};
    draw->AddLine(crossA, crossB, IM_COL32(25, 0, 35, 255), 6.0f);
    draw->AddLine(crossC, crossD, IM_COL32(25, 0, 35, 255), 6.0f);
    draw->AddLine(crossA, crossB, closeColor, 2.5f);
    draw->AddLine(crossC, crossD, closeColor, 2.5f);

    const ImVec2 settings{origin.x + 58.0f, origin.y + 326.0f};
    const ImVec2 settingsSize{480.0f, 350.0f};
    const ImVec2 memo{origin.x + 555.0f, origin.y + 326.0f};
    const ImVec2 memoSize{539.0f, 350.0f};
    DrawPanel(draw, settings, settingsSize);
    DrawPanel(draw, memo, memoSize);
    DrawSettings(settings, settingsSize);
    DrawMemo(memo, memoSize);

    const LaunchState state = g_launchState.load(std::memory_order_acquire);
    const bool valid = !g_config.setSerial || Config::ValidSerial(g_serial.data());
    const bool enabled = state != LaunchState::Running
        && state != LaunchState::Success && valid;
    const char* label = state == LaunchState::Running ? "Launching..."
        : state == LaunchState::Success ? "Loaded"
        : state == LaunchState::Failed ? "Retry" : "Launch";
    if(DrawActionButton(label, {origin.x + 426.0f, origin.y + 685.0f},
        {300.0f, 46.0f}, enabled, draw))
    {
        StartLaunch();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

bool CreateDevice(HWND window)
{
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if(!g_d3d)
        return false;
    g_parameters = {};
    g_parameters.Windowed = TRUE;
    g_parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    g_parameters.EnableAutoDepthStencil = FALSE;
    g_parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT result = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_parameters, &g_device);
    if(FAILED(result))
    {
        result = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_parameters, &g_device);
    }
    return SUCCEEDED(result);
}

void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if(SUCCEEDED(g_device->Reset(&g_parameters)))
        ImGui_ImplDX9_CreateDeviceObjects();
}

void RenderFrame()
{
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawWindow();
    ImGui::EndFrame();
    ImGui::Render();
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(2, 0, 5), 1.0f, 0);
    if(SUCCEEDED(g_device->BeginScene()))
    {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        g_device->EndScene();
    }
    const HRESULT result = g_device->Present(nullptr, nullptr, nullptr, nullptr);
    if(result == D3DERR_DEVICELOST
        && g_device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
    {
        ResetDevice();
    }
}

void DestroyGraphics()
{
    if(g_background) g_background->Release();
    if(g_banner) g_banner->Release();
    if(g_device) g_device->Release();
    if(g_d3d) g_d3d->Release();
    g_background = nullptr;
    g_banner = nullptr;
    g_device = nullptr;
    g_d3d = nullptr;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if(ImGui::GetCurrentContext()
        && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
    {
        return 1;
    }
    switch(message)
    {
    case WM_NCHITTEST:
    {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if(hit != HTCLIENT)
            return hit;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        RECT client{};
        GetClientRect(window, &client);
        if(point.y < 66 && point.x < client.right - 120)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SYSCOMMAND:
        if((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    RuntimeLog::Clear();
    g_directory = LoaderPath::OwnDirectory();
    g_config = Config::Load(g_directory);
    std::memcpy(g_serial.data(), g_config.publicSerial.data(),
        (std::min)(g_config.publicSerial.size(), g_serial.size() - 1));
    Config::Save(g_directory, g_config);

    WNDCLASSEXW windowClass{sizeof(windowClass), CS_CLASSDC, &WindowProc, 0, 0,
        instance, LoadIconW(instance, MAKEINTRESOURCEW(IDI_ICON1)), nullptr,
        nullptr, nullptr, L"DarkFlameLoaderWindow", nullptr};
    RegisterClassExW(&windowClass);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - WindowWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - WindowHeight) / 2;
    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"Dark Flame",
        WS_POPUP, x, y, WindowWidth, WindowHeight, nullptr, nullptr, instance, nullptr);
    if(!g_window || !CreateDevice(g_window))
    {
        MessageBoxW(nullptr, L"DirectX 9 initialization failed.", L"Dark Flame",
            MB_OK | MB_ICONERROR);
        DestroyGraphics();
        UnregisterClassW(windowClass.lpszClassName, instance);
        CoUninitialize();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureStyle();
    ConfigureFonts();
    const bool win32Ready = ImGui_ImplWin32_Init(g_window);
    const bool dx9Ready = win32Ready && ImGui_ImplDX9_Init(g_device);
    const bool texturesReady = dx9Ready
        && LoadTexture(IDR_DARK_FLAME_BACKGROUND, &g_background)
        && LoadTexture(IDR_DARK_FLAME_BANNER, &g_banner);
    if(!texturesReady)
    {
        MessageBoxW(nullptr, L"Dark Flame UI initialization failed.", L"Dark Flame",
            MB_OK | MB_ICONERROR);
        if(dx9Ready)
            ImGui_ImplDX9_Shutdown();
        if(win32Ready)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyGraphics();
        DestroyWindow(g_window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        CoUninitialize();
        return 2;
    }

    const bool shapeReady = ApplyWindowShape(g_window, g_background);
    AppendLog(L"[loader] Dark Flame GUI ready");
    if(!shapeReady)
        AppendLog(L"[warn] transparent window shape initialization failed");
    if(!g_config.serialValid)
        AppendLog(L"[warn] invalid PUBLIC_SERIAL replaced with the default value");
    ShowWindow(g_window, SW_SHOWDEFAULT);
    UpdateWindow(g_window);
    MSG message{};
    while(message.message != WM_QUIT)
    {
        while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(message.message != WM_QUIT)
            RenderFrame();
    }

    if(g_worker.joinable())
    {
        g_worker.request_stop();
        g_worker.join();
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyGraphics();
    UnregisterClassW(windowClass.lpszClassName, instance);
    CoUninitialize();
    return 0;
}
