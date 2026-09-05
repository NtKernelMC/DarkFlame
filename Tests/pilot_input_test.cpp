#include "../Client/lua_bridge_utils.h"
#include "../Client/input_utils.h"
#include <atomic>
#include <cassert>

using LuaBridgeUtil::VirtualKey;
using LuaBridgeUtil::WideAscii;
using InputUtil::KeyPostResult;
using InputUtil::PostKey;

struct Call
{
    std::string key;
    bool down{};
    bool result{};
};

HWND testWindow{};
int directCount{}, directKey{};
bool directDown{}, directResult{true};
std::atomic<DWORD> g_unknownKeyLogTick{};
std::atomic<std::uint32_t> g_keyEmulationLogs{};

namespace Log
{
void Write(const std::wstring&) {}
}

HWND GameWindow() { return testWindow; }
std::string LuaText(void* lua, int, int) { return static_cast<Call*>(lua)->key; }
int DarkFlameLuaToBoolean(void* lua, int) { return static_cast<Call*>(lua)->down; }
int PushDirectResult(void* lua, bool value)
{
    static_cast<Call*>(lua)->result = value;
    return 1;
}

bool GuiEmulateKey(int key, bool down)
{
    ++directCount;
    directKey = key;
    directDown = down;
    return directResult;
}

int __cdecl DirectEmulateMouseButton(void* lua);
#include "../.codex-temp-dia2dump/pilot-input-dispatch.h"

void ExpectMessage(UINT message, WPARAM key, bool release)
{
    MSG actual{};
    assert(PeekMessageW(&actual, testWindow, message, message, PM_REMOVE));
    assert(actual.wParam == key);
    if(message == WM_KEYDOWN || message == WM_KEYUP)
    {
        assert(((actual.lParam >> 31) & 1) == release);
        assert(((actual.lParam >> 30) & 1) == release);
        assert((actual.lParam & 0xFFFF) == 1);
    }
}

int main()
{
    testWindow = CreateWindowW(L"STATIC", L"Input test", 0, 0, 0, 1, 1,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(testWindow);
    for(const char* key : {"w", "s", "a", "d", "SPACE", "ENTER", "LSHIFT"})
    {
        for(bool down : {true, false})
        {
            Call call{key, down};
            const int before = directCount;
            assert(DirectEmulateKey(&call) == 1 && call.result);
            assert(directCount == before + 1 && directKey == VirtualKey(key) && directDown == down);
            ExpectMessage(down ? WM_KEYDOWN : WM_KEYUP, VirtualKey(key), !down);
        }
    }
    const int keyboardCalls = directCount;
    for(const char* button : {"rmb", "right", "lmb", "left"})
    {
        const bool right = std::string(button) == "rmb" || std::string(button) == "right";
        for(bool down : {true, false})
        {
            Call call{button, down};
            const int result = std::string(button) == "rmb"
                ? DirectEmulateKey(&call) : DirectEmulateMouseButton(&call);
            assert(result == 1 && call.result && directCount == keyboardCalls);
            ExpectMessage(right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP), down ? (right ? MK_RBUTTON : MK_LBUTTON) : 0, !down);
        }
    }
    Call invalid{"unsupported", true};
    DirectEmulateKey(&invalid);
    assert(!invalid.result && directCount == keyboardCalls);
    DestroyWindow(testWindow);
    testWindow = nullptr;
    Call mouse{"rmb", true};
    DirectEmulateKey(&mouse);
    assert(!mouse.result && directCount == keyboardCalls);
    Call key{"w", true};
    DirectEmulateKey(&key);
    assert(key.result && directCount == keyboardCalls + 1);
    directResult = false;
    DirectEmulateKey(&key);
    assert(!key.result);
    std::puts("Keyboard dispatch, RMB alias, original JBK mouse API: OK");
}
