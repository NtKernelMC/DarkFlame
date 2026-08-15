#include "gui.h"

#include "logger.h"
#include "resource.h"

#include <MinHook.h>
#define DIRECTINPUT_VERSION 0x0800
#include <d3d9.h>
#include <dinput.h>
#include <wincodec.h>

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_dx9.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/ImGuiColorTextEdit/TextEditor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window,
    UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr float CanvasWidth = 1536.0f;
constexpr float CanvasHeight = 1024.0f;
constexpr GUID DirectInput8WGuid = {0xBF798031, 0x483A, 0x4DA2,
    {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID SystemMouseGuid = {0x6F1D2B60, 0xD5A0, 0x11CF,
    {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*,
    const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND,
    DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT(__stdcall*)(IDirect3D9Ex*, UINT, D3DDEVTYPE,
    HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using PresentExFn = HRESULT(__stdcall*)(IDirect3DDevice9Ex*, const RECT*,
    const RECT*, HWND, const RGNDATA*, DWORD);
using ResetExFn = HRESULT(__stdcall*)(IDirect3DDevice9Ex*,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID,
    void**, IUnknown*);
using CreateInputDeviceFn = HRESULT(__stdcall*)(IDirectInput8W*, REFGUID,
    IDirectInputDevice8W**, IUnknown*);
using GetDeviceStateFn = HRESULT(__stdcall*)(IDirectInputDevice8W*, DWORD, void*);
using GetDeviceDataFn = HRESULT(__stdcall*)(IDirectInputDevice8W*, DWORD,
    DIDEVICEOBJECTDATA*, DWORD*, DWORD);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

HMODULE g_module{};
PresentFn g_present{};
ResetFn g_reset{};
Direct3DCreate9Fn g_create9{};
Direct3DCreate9ExFn g_create9Ex{};
CreateDeviceFn g_createDevice{};
CreateDeviceExFn g_createDeviceEx{};
PresentExFn g_presentEx{};
ResetExFn g_resetEx{};
DirectInput8CreateFn g_directInput8Create{};
CreateInputDeviceFn g_createInputDevice{};
GetDeviceStateFn g_getDeviceState{};
GetDeviceDataFn g_getDeviceData{};
SetCursorPosFn g_setCursorPos{};
ClipCursorFn g_clipCursor{};
HWND g_window{};
WNDPROC g_oldWndProc{};
IDirect3DTexture9* g_background{};
IDirect3DTexture9* g_banner{};
ImFont* g_titleFont{};
ImFont* g_tabFont{};
ImFont* g_buttonFont{};
ImFont* g_codeFont{};
std::unique_ptr<TextEditor> g_editor;
std::array<char, 65536> g_events{};
std::mutex g_bridgeMutex;
std::string g_pendingLua;
std::string g_pendingResource;
std::string g_eventText;
std::vector<GuiLuaThread> g_threads;
std::vector<GuiLuaThread> g_visibleThreads;
char g_targetResource[128]{"province_ac"};
std::uintptr_t g_selectedThread{};
std::uintptr_t g_pendingUnload{};
bool g_luaPending{};
bool g_eventsDirty{};
bool g_threadsDirty{};
std::atomic_bool g_started{};
std::atomic_bool g_visible{};
std::atomic_long g_factoryState{};
std::atomic_long g_factoryExState{};
std::atomic_long g_deviceState{};
std::atomic_long g_deviceExState{};
std::atomic_long g_inputFactoryState{};
std::atomic_long g_inputDeviceState{};
bool g_initialized{};
int g_activeTab{};

ImTextureRef Texture(IDirect3DTexture9* texture)
{
    return ImTextureRef(reinterpret_cast<void*>(texture));
}

bool InputMessage(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        || (message >= WM_KEYFIRST && message <= WM_KEYLAST)
        || message == WM_INPUT;
}

LRESULT CALLBACK GuiWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if(g_visible.load() && ImGui::GetCurrentContext())
    {
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        if(InputMessage(message))
            return 1;
    }
    return g_oldWndProc
        ? CallWindowProcW(g_oldWndProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

BOOL WINAPI HookSetCursorPos(int x, int y)
{
    return g_visible.load() ? TRUE : g_setCursorPos(x, y);
}

BOOL WINAPI HookClipCursor(const RECT* rectangle)
{
    return g_visible.load() ? g_clipCursor(nullptr) : g_clipCursor(rectangle);
}

HRESULT __stdcall HookGetDeviceState(IDirectInputDevice8W* device,
    DWORD size, void* data)
{
    const HRESULT result = g_getDeviceState(device, size, data);
    if(g_visible.load() && SUCCEEDED(result) && data && size)
        std::memset(data, 0, size);
    return result;
}

HRESULT __stdcall HookGetDeviceData(IDirectInputDevice8W* device,
    DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags)
{
    const HRESULT result = g_getDeviceData(device, objectSize, data, count, flags);
    if(g_visible.load() && SUCCEEDED(result) && count)
        *count = 0;
    return result;
}

bool InstallInputDeviceHooks(IDirectInputDevice8W* device)
{
    long expected{};
    if(!device || !g_inputDeviceState.compare_exchange_strong(expected, 1))
        return expected == 2;
    auto** table = *reinterpret_cast<void***>(device);
    void* stateTarget = table[9];
    void* dataTarget = table[10];
    const MH_STATUS stateCreated = MH_CreateHook(stateTarget,
        reinterpret_cast<void*>(&HookGetDeviceState),
        reinterpret_cast<void**>(&g_getDeviceState));
    const MH_STATUS dataCreated = MH_CreateHook(dataTarget,
        reinterpret_cast<void*>(&HookGetDeviceData),
        reinterpret_cast<void**>(&g_getDeviceData));
    const bool ready = stateCreated == MH_OK && dataCreated == MH_OK
        && MH_EnableHook(stateTarget) == MH_OK
        && MH_EnableHook(dataTarget) == MH_OK;
    if(!ready)
    {
        if(stateCreated == MH_OK)
        {
            MH_DisableHook(stateTarget);
            MH_RemoveHook(stateTarget);
        }
        if(dataCreated == MH_OK)
        {
            MH_DisableHook(dataTarget);
            MH_RemoveHook(dataTarget);
        }
        g_getDeviceState = nullptr;
        g_getDeviceData = nullptr;
    }
    g_inputDeviceState.store(ready ? 2 : 0);
    return ready;
}

HRESULT __stdcall HookCreateInputDevice(IDirectInput8W* api, REFGUID guid,
    IDirectInputDevice8W** output, IUnknown* outer)
{
    const HRESULT result = g_createInputDevice(api, guid, output, outer);
    if(SUCCEEDED(result) && output && *output)
        InstallInputDeviceHooks(*output);
    return result;
}

bool InstallInputFactoryHook(IDirectInput8W* api)
{
    long expected{};
    if(!api || !g_inputFactoryState.compare_exchange_strong(expected, 1))
        return expected == 2;
    auto** table = *reinterpret_cast<void***>(api);
    void* target = table[3];
    const MH_STATUS created = MH_CreateHook(target,
        reinterpret_cast<void*>(&HookCreateInputDevice),
        reinterpret_cast<void**>(&g_createInputDevice));
    const bool ready = created == MH_OK && MH_EnableHook(target) == MH_OK;
    if(!ready)
    {
        if(created == MH_OK)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        g_createInputDevice = nullptr;
    }
    g_inputFactoryState.store(ready ? 2 : 0);
    return ready;
}

HRESULT WINAPI HookDirectInput8Create(HINSTANCE instance, DWORD version,
    REFIID iid, void** output, IUnknown* outer)
{
    const HRESULT result = g_directInput8Create(instance, version, iid, output, outer);
    if(SUCCEEDED(result) && output && *output)
        InstallInputFactoryHook(static_cast<IDirectInput8W*>(*output));
    return result;
}

void InstallGameInputHooks()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* setCursorPos = user32 ? reinterpret_cast<void*>(
        GetProcAddress(user32, "SetCursorPos")) : nullptr;
    void* clipCursor = user32 ? reinterpret_cast<void*>(
        GetProcAddress(user32, "ClipCursor")) : nullptr;
    const bool cursorReady = setCursorPos && clipCursor
        && MH_CreateHook(setCursorPos, reinterpret_cast<void*>(&HookSetCursorPos),
            reinterpret_cast<void**>(&g_setCursorPos)) == MH_OK
        && MH_CreateHook(clipCursor, reinterpret_cast<void*>(&HookClipCursor),
            reinterpret_cast<void**>(&g_clipCursor)) == MH_OK
        && MH_EnableHook(setCursorPos) == MH_OK
        && MH_EnableHook(clipCursor) == MH_OK;
    Log::Write(cursorReady
        ? L"[gui] cursor recenter blocking installed"
        : L"[gui] cursor recenter blocking failed");
}

bool LoadTexture(IDirect3DDevice9* device, int resourceId,
    IDirect3DTexture9** output)
{
    if(!device || !output)
        return false;
    HRSRC resource = FindResourceW(g_module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(g_module, resource) : nullptr;
    const DWORD resourceSize = resource ? SizeofResource(g_module, resource) : 0;
    auto* bytes = loaded ? static_cast<BYTE*>(LockResource(loaded)) : nullptr;
    if(!bytes || !resourceSize)
        return false;

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
            && SUCCEEDED(device->CreateTexture(width, height, 1, 0,
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
    if(comResult == S_OK || comResult == S_FALSE)
        CoUninitialize();
    if(!success && texture)
    {
        texture->Release();
        texture = nullptr;
    }
    *output = texture;
    return success;
}

void ConfigureEditor()
{
    g_editor = std::make_unique<TextEditor>();
    g_editor->SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    g_editor->SetTabSize(4);
    auto palette = TextEditor::GetDarkPalette();
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Default)] = IM_COL32(226, 218, 242, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Keyword)] = IM_COL32(225, 76, 255, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Number)] = IM_COL32(122, 190, 255, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::String)] = IM_COL32(222, 218, 108, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::CharLiteral)] = IM_COL32(222, 218, 108, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Punctuation)] = IM_COL32(203, 160, 255, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Identifier)] = IM_COL32(216, 208, 235, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::KnownIdentifier)] = IM_COL32(93, 206, 255, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Comment)] = IM_COL32(112, 86, 143, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::MultiLineComment)] = IM_COL32(112, 86, 143, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Background)] = IM_COL32(4, 3, 12, 238);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Cursor)] = IM_COL32(244, 205, 255, 255);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::Selection)] = IM_COL32(116, 28, 177, 170);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::LineNumber)] = IM_COL32(111, 91, 133, 230);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::CurrentLineFill)] = IM_COL32(37, 12, 55, 190);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::CurrentLineFillInactive)] = IM_COL32(18, 8, 28, 160);
    palette[static_cast<unsigned>(TextEditor::PaletteIndex::CurrentLineEdge)] = IM_COL32(154, 54, 223, 200);
    g_editor->SetPalette(palette);
    g_editor->SetText(
        "--[[\n"
        "  Dark Flame Lua Injector\n"
        "  Write or paste your Lua script below.\n"
        "]]--\n\n"
        "-- Example\n"
        "local player = localPlayer\n"
        "outputChatBox(\"Dark Flame is active.\")\n\n"
        "-- Your script here...\n");
}

void ConfigureFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFont* fallback = io.Fonts->AddFontDefault();
    io.FontDefault = fallback;
    g_titleFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 29.0f);
    g_tabFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 27.0f);
    g_buttonFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 55.0f);
    g_codeFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 20.0f);
    if(!g_titleFont) g_titleFont = fallback;
    if(!g_tabFont) g_tabFont = fallback;
    if(!g_buttonFont) g_buttonFont = fallback;
    if(!g_codeFont) g_codeFont = fallback;
}

void ConfigureStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 3.0f;
    style.ScrollbarSize = 13.0f;
    style.Colors[ImGuiCol_Text] = ImVec4(0.88f, 0.83f, 0.96f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.018f, 0.012f, 0.04f, 0.96f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.01f, 0.04f, 0.9f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.32f, 0.08f, 0.52f, 0.9f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.58f, 0.12f, 0.88f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.75f, 0.22f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.01f, 0.006f, 0.025f, 0.92f);
}

bool InitializeGui(IDirect3DDevice9* device)
{
    D3DDEVICE_CREATION_PARAMETERS parameters{};
    if(!device || FAILED(device->GetCreationParameters(&parameters))
        || !parameters.hFocusWindow)
        return false;
    g_window = parameters.hFocusWindow;
    if(!LoadTexture(device, IDR_DARK_FLAME_BACKGROUND, &g_background)
        || !LoadTexture(device, IDR_DARK_FLAME_BANNER, &g_banner))
    {
        if(g_background) g_background->Release();
        if(g_banner) g_banner->Release();
        g_background = nullptr;
        g_banner = nullptr;
        return false;
    }
    const auto releaseTextures = []
    {
        if(g_background) g_background->Release();
        if(g_banner) g_banner->Release();
        g_background = nullptr;
        g_banner = nullptr;
    };
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureStyle();
    ConfigureFonts();
    ConfigureEditor();
    if(!ImGui_ImplWin32_Init(g_window))
    {
        ImGui::DestroyContext();
        releaseTextures();
        return false;
    }
    if(!ImGui_ImplDX9_Init(device))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        releaseTextures();
        return false;
    }
    g_oldWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GuiWndProc)));
    if(!g_oldWndProc)
    {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        releaseTextures();
        return false;
    }
    g_initialized = true;
    Log::Write(L"[gui] ImGui DX9 menu ready; toggle with Shift+R");
    return true;
}

void DrawFlame(ImDrawList* draw, ImVec2 center, float size)
{
    const ImU32 glow = IM_COL32(123, 24, 220, 115);
    const ImU32 purple = IM_COL32(204, 65, 255, 255);
    const ImU32 white = IM_COL32(249, 218, 255, 255);
    draw->AddCircleFilled(center, size * 0.55f, glow, 24);
    const ImVec2 outer[] = {
        {center.x, center.y - size},
        {center.x + size * 0.62f, center.y - size * 0.08f},
        {center.x + size * 0.35f, center.y + size * 0.7f},
        {center.x, center.y + size},
        {center.x - size * 0.48f, center.y + size * 0.45f},
        {center.x - size * 0.6f, center.y - size * 0.16f}};
    draw->AddConvexPolyFilled(outer, 6, purple);
    const ImVec2 inner[] = {
        {center.x + size * 0.12f, center.y - size * 0.42f},
        {center.x + size * 0.3f, center.y + size * 0.18f},
        {center.x, center.y + size * 0.55f},
        {center.x - size * 0.18f, center.y + size * 0.08f}};
    draw->AddConvexPolyFilled(inner, 4, white);
}

void DrawCodeIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    draw->AddLine({center.x - size * 0.15f, center.y - size * 0.65f},
        {center.x - size * 0.75f, center.y}, color, 2.2f);
    draw->AddLine({center.x - size * 0.75f, center.y},
        {center.x - size * 0.15f, center.y + size * 0.65f}, color, 2.2f);
    draw->AddLine({center.x + size * 0.15f, center.y - size * 0.65f},
        {center.x + size * 0.75f, center.y}, color, 2.2f);
    draw->AddLine({center.x + size * 0.75f, center.y},
        {center.x + size * 0.15f, center.y + size * 0.65f}, color, 2.2f);
    draw->AddLine({center.x + size * 0.18f, center.y - size * 0.88f},
        {center.x - size * 0.18f, center.y + size * 0.88f}, color, 2.0f);
}

void DrawTab(const char* id, const char* label, int tab, ImVec2 position,
    ImVec2 size, bool active, ImDrawList* draw, float scale)
{
    ImGui::SetCursorScreenPos(position);
    if(ImGui::InvisibleButton(id, size))
        g_activeTab = tab;
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 color = active ? IM_COL32(250, 225, 255, 255)
        : hovered ? IM_COL32(199, 157, 224, 255) : IM_COL32(142, 125, 159, 255);
    if(active)
    {
        draw->AddRectFilled({position.x, position.y + size.y - 5.0f * scale},
            {position.x + size.x, position.y + size.y}, IM_COL32(203, 53, 255, 235));
        draw->AddRectFilled({position.x + 12.0f * scale,
            position.y + size.y - 9.0f * scale},
            {position.x + size.x - 12.0f * scale,
            position.y + size.y - 5.0f * scale},
            IM_COL32(135, 24, 220, 90));
    }
    const float fontSize = g_tabFont->LegacySize * scale;
    const ImVec2 textSize = g_tabFont->CalcTextSizeA(fontSize,
        FLT_MAX, 0.0f, label);
    draw->AddText(g_tabFont, fontSize,
        {position.x + (size.x - textSize.x) * 0.5f,
        position.y + (size.y - textSize.y) * 0.48f}, color, label);
}

bool DrawActionButton(const char* id, const char* label, ImVec2 position,
    ImVec2 size, ImDrawList* draw, float scale)
{
    ImGui::SetCursorScreenPos(position);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 glow = hovered ? IM_COL32(202, 49, 255, 110)
        : IM_COL32(132, 28, 216, 72);
    const ImU32 border = hovered ? IM_COL32(242, 124, 255, 255)
        : IM_COL32(192, 67, 245, 245);
    draw->AddRectFilled(position, {position.x + size.x, position.y + size.y},
        IM_COL32(13, 4, 27, 238), 14.0f * scale);
    draw->AddRect(position, {position.x + size.x, position.y + size.y}, glow,
        14.0f * scale, 0, 10.0f * scale);
    draw->AddRect({position.x + 5.0f * scale, position.y + 5.0f * scale},
        {position.x + size.x - 5.0f * scale,
        position.y + size.y - 5.0f * scale}, border,
        12.0f * scale, 0, 3.0f * scale);
    const float fontSize = g_buttonFont->LegacySize * scale;
    const ImVec2 textSize = g_buttonFont->CalcTextSizeA(fontSize,
        FLT_MAX, 0.0f, label);
    draw->AddText(g_buttonFont, fontSize,
        {position.x + (size.x - textSize.x) * 0.5f,
        position.y + (size.y - textSize.y) * 0.43f},
        IM_COL32(252, 238, 255, 255), label);
    for(int side : {-1, 1})
    {
        const float base = side < 0 ? position.x + 72.0f * scale
            : position.x + size.x - 72.0f * scale;
        for(int index = 0; index < 3; ++index)
        {
            const float offset = index * 14.0f * scale * side;
            draw->AddLine({base + offset - 9.0f * scale * side,
                position.y + size.y * 0.36f},
                {base + offset, position.y + size.y * 0.5f}, border, 3.0f * scale);
            draw->AddLine({base + offset, position.y + size.y * 0.5f},
                {base + offset - 9.0f * scale * side,
                position.y + size.y * 0.64f}, border, 3.0f * scale);
        }
    }
    return clicked;
}

void QueueEditorCode()
{
    if(!g_editor)
        return;
    std::scoped_lock lock(g_bridgeMutex);
    g_pendingLua = g_editor->GetText();
    g_pendingResource = g_targetResource[0] ? g_targetResource : "province_ac";
    g_luaPending = true;
}

void SyncEvents()
{
    std::scoped_lock lock(g_bridgeMutex);
    if(!g_eventsDirty)
        return;
    const std::size_t length = std::min(g_eventText.size(), g_events.size() - 1);
    std::memcpy(g_events.data(), g_eventText.data(), length);
    g_events[length] = '\0';
    g_eventsDirty = false;
}

void SyncThreads()
{
    std::scoped_lock lock(g_bridgeMutex);
    if(!g_threadsDirty)
        return;
    g_visibleThreads = g_threads;
    g_threadsDirty = false;
    if(g_selectedThread && std::none_of(g_visibleThreads.begin(),
        g_visibleThreads.end(), [](const GuiLuaThread& thread)
        {
            return thread.id == g_selectedThread;
        }))
    {
        g_selectedThread = 0;
    }
}

void DrawThreadList(ImVec2 position, ImVec2 size, float scale)
{
    SyncThreads();
    ImGui::SetCursorScreenPos(position);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(4, 3, 12, 238));
    ImGui::BeginChild("##lua_threads", size, ImGuiChildFlags_Borders,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextColored(ImVec4(0.75f, 0.34f, 1.0f, 1.0f),
        "Timestamp              Thread ID        Resource");
    ImGui::Separator();
    for(const GuiLuaThread& thread : g_visibleThreads)
    {
        char label[384]{};
        std::snprintf(label, sizeof(label), "%s            0x%08lX        %s##%08lX",
            thread.timestamp.c_str(), static_cast<unsigned long>(thread.id),
            thread.resource.c_str(), static_cast<unsigned long>(thread.id));
        if(ImGui::Selectable(label, g_selectedThread == thread.id,
            ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, 28.0f * scale)))
        {
            g_selectedThread = thread.id;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RenderMenu()
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true;
    const float scale = std::min(io.DisplaySize.x / CanvasWidth,
        io.DisplaySize.y / CanvasHeight) * 0.94f;
    const ImVec2 size(CanvasWidth * scale, CanvasHeight * scale);
    const ImVec2 centered((io.DisplaySize.x - size.x) * 0.5f,
        (io.DisplaySize.y - size.y) * 0.5f);

    ImGui::SetNextWindowPos(centered, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##dark_flame_menu", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetWindowPos();
    const auto point = [&](float x, float y)
    {
        return ImVec2(origin.x + x * scale, origin.y + y * scale);
    };
    const auto extent = [&](float x, float y)
    {
        return ImVec2(x * scale, y * scale);
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddImage(Texture(g_background), origin,
        {origin.x + size.x, origin.y + size.y});
    draw->AddImage(Texture(g_banner), point(80.0f, 113.0f), point(1456.0f, 404.0f));

    DrawFlame(draw, point(94.0f, 61.0f), 22.0f * scale);
    const char* title = "Dark Flame by DroidZero";
    const float titleFontSize = g_titleFont->LegacySize * scale;
    const ImVec2 titleSize = g_titleFont->CalcTextSizeA(titleFontSize,
        FLT_MAX, 0.0f, title);
    draw->AddText(g_titleFont, titleFontSize,
        {origin.x + (size.x - titleSize.x) * 0.5f, point(0.0f, 45.0f).y},
        IM_COL32(245, 228, 255, 255), title);

    DrawCodeIcon(draw, point(111.0f, 453.0f), 15.0f * scale,
        IM_COL32(201, 45, 255, 255));
    DrawTab("##tab_lua", "Lua Injector", 0, point(135.0f, 421.0f),
        extent(280.0f, 65.0f), g_activeTab == 0, draw, scale);
    DrawTab("##tab_events", "Event Monitor", 1, point(435.0f, 421.0f),
        extent(280.0f, 65.0f), g_activeTab == 1, draw, scale);
    DrawTab("##tab_threads", "Lua Threads", 2, point(735.0f, 421.0f),
        extent(280.0f, 65.0f), g_activeTab == 2, draw, scale);

    const ImVec2 contentPosition = point(105.0f, 495.0f);
    const ImVec2 contentSize = extent(1326.0f,
        g_activeTab == 0 ? 285.0f : 310.0f);
    ImGui::SetCursorScreenPos(contentPosition);
    ImGui::PushFont(g_codeFont, g_codeFont->LegacySize * scale);
    if(g_activeTab == 0)
    {
        g_editor->Render("##lua_editor", contentSize, false);
        const ImVec2 labelPosition = point(110.0f, 799.0f);
        draw->AddText(g_tabFont, g_tabFont->LegacySize * scale, labelPosition,
            IM_COL32(213, 183, 236, 255), "Target resource:");
        ImGui::SetCursorScreenPos(point(345.0f, 796.0f));
        ImGui::SetNextItemWidth(680.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(7, 3, 18, 245));
        ImGui::InputText("##target_resource", g_targetResource,
            sizeof(g_targetResource));
        ImGui::PopStyleColor();
    }
    else if(g_activeTab == 1)
    {
        SyncEvents();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(4, 3, 12, 238));
        ImGui::InputTextMultiline("##event_monitor", g_events.data(), g_events.size(),
            contentSize, ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
        ImGui::PopStyleColor();
    }
    else
        DrawThreadList(contentPosition, contentSize, scale);
    ImGui::PopFont();

    if(g_activeTab == 0 && DrawActionButton("##inject_visual", "Inject",
        point(410.0f, 861.0f), extent(716.0f, 104.0f), draw, scale))
    {
        QueueEditorCode();
    }
    else if(g_activeTab == 2 && DrawActionButton("##unload_visual", "Unload",
        point(410.0f, 861.0f), extent(716.0f, 104.0f), draw, scale)
        && g_selectedThread)
    {
        std::scoped_lock lock(g_bridgeMutex);
        g_pendingUnload = g_selectedThread;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void RenderFrame(IDirect3DDevice9* device)
{
    if(!g_initialized && !InitializeGui(device))
        return;
    static bool toggleHeld{};
    const bool toggleDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        && (GetAsyncKeyState('R') & 0x8000);
    if(toggleDown && !toggleHeld)
    {
        const bool visible = !g_visible.load();
        g_visible.store(visible);
        if(visible && g_clipCursor)
            g_clipCursor(nullptr);
    }
    toggleHeld = toggleDown;
    if(!g_visible.load())
    {
        ImGui::GetIO().MouseDrawCursor = false;
        return;
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderMenu();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

HRESULT __stdcall HookPresent(IDirect3DDevice9* device, const RECT* source,
    const RECT* destination, HWND window, const RGNDATA* dirty)
{
    RenderFrame(device);
    return g_present(device, source, destination, window, dirty);
}

HRESULT __stdcall HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    if(g_initialized)
        ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT result = g_reset(device, parameters);
    if(g_initialized && SUCCEEDED(result))
        ImGui_ImplDX9_CreateDeviceObjects();
    return result;
}

HRESULT __stdcall HookPresentEx(IDirect3DDevice9Ex* device, const RECT* source,
    const RECT* destination, HWND window, const RGNDATA* dirty, DWORD flags)
{
    RenderFrame(device);
    return g_presentEx(device, source, destination, window, dirty, flags);
}

HRESULT __stdcall HookResetEx(IDirect3DDevice9Ex* device,
    D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* mode)
{
    if(g_initialized)
        ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT result = g_resetEx(device, parameters, mode);
    if(g_initialized && SUCCEEDED(result))
        ImGui_ImplDX9_CreateDeviceObjects();
    return result;
}

bool InstallDeviceHooks(IDirect3DDevice9* device)
{
    long expected{};
    if(!g_deviceState.compare_exchange_strong(expected, 1))
        return expected == 2;
    auto** table = *reinterpret_cast<void***>(device);
    void* resetTarget = table[16];
    void* presentTarget = table[17];
    const MH_STATUS resetCreated = MH_CreateHook(resetTarget,
        reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_reset));
    const MH_STATUS presentCreated = MH_CreateHook(presentTarget,
        reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_present));
    const bool ready = resetCreated == MH_OK && presentCreated == MH_OK
        && MH_EnableHook(resetTarget) == MH_OK && MH_EnableHook(presentTarget) == MH_OK;
    if(!ready)
    {
        if(resetCreated == MH_OK)
        {
            MH_DisableHook(resetTarget);
            MH_RemoveHook(resetTarget);
        }
        if(presentCreated == MH_OK)
        {
            MH_DisableHook(presentTarget);
            MH_RemoveHook(presentTarget);
        }
        g_reset = nullptr;
        g_present = nullptr;
    }
    g_deviceState.store(ready ? 2 : 0);
    Log::Write(ready ? L"[gui] real DX9 device hooks installed"
        : L"[gui] real DX9 device hook installation failed");
    return ready;
}

bool InstallDeviceExHooks(IDirect3DDevice9Ex* device)
{
    long expected{};
    if(!g_deviceExState.compare_exchange_strong(expected, 1))
        return expected == 2;
    auto** table = *reinterpret_cast<void***>(device);
    void* presentTarget = table[121];
    void* resetTarget = table[132];
    const MH_STATUS presentCreated = MH_CreateHook(presentTarget,
        reinterpret_cast<void*>(&HookPresentEx), reinterpret_cast<void**>(&g_presentEx));
    const MH_STATUS resetCreated = MH_CreateHook(resetTarget,
        reinterpret_cast<void*>(&HookResetEx), reinterpret_cast<void**>(&g_resetEx));
    const bool ready = presentCreated == MH_OK && resetCreated == MH_OK
        && MH_EnableHook(presentTarget) == MH_OK && MH_EnableHook(resetTarget) == MH_OK;
    if(!ready)
    {
        if(presentCreated == MH_OK)
        {
            MH_DisableHook(presentTarget);
            MH_RemoveHook(presentTarget);
        }
        if(resetCreated == MH_OK)
        {
            MH_DisableHook(resetTarget);
            MH_RemoveHook(resetTarget);
        }
        g_presentEx = nullptr;
        g_resetEx = nullptr;
    }
    g_deviceExState.store(ready ? 2 : 0);
    Log::Write(ready ? L"[gui] real DX9Ex device hooks installed"
        : L"[gui] real DX9Ex device hook installation failed");
    return ready;
}

HRESULT __stdcall HookCreateDevice(IDirect3D9* api, UINT adapter,
    D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** output)
{
    const HRESULT result = g_createDevice(api, adapter, type, window, flags,
        parameters, output);
    if(SUCCEEDED(result) && output && *output)
        InstallDeviceHooks(*output);
    return result;
}

HRESULT __stdcall HookCreateDeviceEx(IDirect3D9Ex* api, UINT adapter,
    D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** output)
{
    const HRESULT result = g_createDeviceEx(api, adapter, type, window, flags,
        parameters, mode, output);
    if(SUCCEEDED(result) && output && *output)
    {
        InstallDeviceHooks(*output);
        InstallDeviceExHooks(*output);
    }
    return result;
}

bool InstallFactoryHooks(IDirect3D9* api)
{
    long expected{};
    if(!g_factoryState.compare_exchange_strong(expected, 1))
        return expected == 2;
    auto** table = *reinterpret_cast<void***>(api);
    void* target = table[16];
    const MH_STATUS created = MH_CreateHook(target,
        reinterpret_cast<void*>(&HookCreateDevice),
        reinterpret_cast<void**>(&g_createDevice));
    const bool ready = created == MH_OK && MH_EnableHook(target) == MH_OK;
    if(!ready)
    {
        if(created == MH_OK)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        g_createDevice = nullptr;
    }
    g_factoryState.store(ready ? 2 : 0);
    Log::Write(ready ? L"[gui] IDirect3D9::CreateDevice hooked"
        : L"[gui] IDirect3D9::CreateDevice hook failed");
    return ready;
}

bool InstallFactoryExHooks(IDirect3D9Ex* api)
{
    long expected{};
    if(!g_factoryExState.compare_exchange_strong(expected, 1))
        return expected == 2;
    auto** table = *reinterpret_cast<void***>(api);
    void* target = table[20];
    const MH_STATUS created = MH_CreateHook(target,
        reinterpret_cast<void*>(&HookCreateDeviceEx),
        reinterpret_cast<void**>(&g_createDeviceEx));
    const bool ready = created == MH_OK && MH_EnableHook(target) == MH_OK;
    if(!ready)
    {
        if(created == MH_OK)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        g_createDeviceEx = nullptr;
    }
    g_factoryExState.store(ready ? 2 : 0);
    Log::Write(ready ? L"[gui] IDirect3D9Ex::CreateDeviceEx hooked"
        : L"[gui] IDirect3D9Ex::CreateDeviceEx hook failed");
    return ready;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
{
    IDirect3D9* api = g_create9(sdkVersion);
    if(api) InstallFactoryHooks(api);
    return api;
}

HRESULT WINAPI HookDirect3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** output)
{
    const HRESULT result = g_create9Ex(sdkVersion, output);
    if(SUCCEEDED(result) && output && *output)
    {
        InstallFactoryHooks(*output);
        InstallFactoryExHooks(*output);
    }
    return result;
}

DWORD WINAPI HookThread(void*)
{
    InstallGameInputHooks();
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if(!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
    void* create9 = d3d9 ? reinterpret_cast<void*>(
        GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    void* create9Ex = d3d9 ? reinterpret_cast<void*>(
        GetProcAddress(d3d9, "Direct3DCreate9Ex")) : nullptr;
    bool standardCreated{}, standard{}, extendedCreated{}, extended{};
    if(create9)
    {
        standardCreated = MH_CreateHook(create9,
            reinterpret_cast<void*>(&HookDirect3DCreate9),
            reinterpret_cast<void**>(&g_create9)) == MH_OK;
        standard = standardCreated && MH_EnableHook(create9) == MH_OK;
    }
    if(create9Ex)
    {
        extendedCreated = MH_CreateHook(create9Ex,
            reinterpret_cast<void*>(&HookDirect3DCreate9Ex),
            reinterpret_cast<void**>(&g_create9Ex)) == MH_OK;
        extended = extendedCreated && MH_EnableHook(create9Ex) == MH_OK;
    }
    if(!standard && standardCreated)
    {
        MH_DisableHook(create9);
        MH_RemoveHook(create9);
        g_create9 = nullptr;
    }
    if(!extended && extendedCreated)
    {
        MH_DisableHook(create9Ex);
        MH_RemoveHook(create9Ex);
        g_create9Ex = nullptr;
    }
    Log::Write(standard || extended
        ? L"[gui] DX9 factory hooks installed; waiting for the real game device"
        : L"[gui] DX9 factory hook installation failed");
    return 0;
}
}

bool StartGui(HMODULE module)
{
    if(!module || g_started.exchange(true))
        return false;
    g_module = module;
    HANDLE thread = CreateThread(nullptr, 0, &HookThread, nullptr, 0, nullptr);
    if(!thread)
        return false;
    CloseHandle(thread);
    return true;
}

bool GuiVisible()
{
    return g_visible.load();
}

bool GuiTakeLuaCode(std::string& code, std::string& resource)
{
    std::scoped_lock lock(g_bridgeMutex);
    if(!g_luaPending)
        return false;
    code = std::move(g_pendingLua);
    resource = std::move(g_pendingResource);
    g_pendingLua.clear();
    g_pendingResource.clear();
    g_luaPending = false;
    return true;
}

bool GuiTakeUnloadThread(std::uintptr_t& id)
{
    std::scoped_lock lock(g_bridgeMutex);
    if(!g_pendingUnload)
        return false;
    id = g_pendingUnload;
    g_pendingUnload = 0;
    return true;
}

void GuiAddLuaThread(const GuiLuaThread& thread)
{
    std::scoped_lock lock(g_bridgeMutex);
    g_threads.push_back(thread);
    g_threadsDirty = true;
}

void GuiRemoveLuaThread(std::uintptr_t id)
{
    std::scoped_lock lock(g_bridgeMutex);
    const auto end = std::remove_if(g_threads.begin(), g_threads.end(),
        [id](const GuiLuaThread& thread)
    {
        return thread.id == id;
    });
    g_threads.erase(end, g_threads.end());
    g_threadsDirty = true;
}

void GuiClearLuaThreads()
{
    std::scoped_lock lock(g_bridgeMutex);
    g_threads.clear();
    g_pendingUnload = 0;
    g_threadsDirty = true;
}

void GuiAppendEvent(std::string_view event)
{
    if(event.empty())
        return;
    std::scoped_lock lock(g_bridgeMutex);
    if(!g_eventText.empty())
        g_eventText.push_back('\n');
    g_eventText.append(event);
    constexpr std::size_t MaxEventText = 64000;
    if(g_eventText.size() > MaxEventText)
    {
        const std::size_t excess = g_eventText.size() - MaxEventText;
        const std::size_t newline = g_eventText.find('\n', excess);
        g_eventText.erase(0, newline == std::string::npos ? excess : newline + 1);
    }
    g_eventsDirty = true;
}
