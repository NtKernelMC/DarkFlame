#include "../Client/lua_bridge_utils.h"
#include "../Client/input_utils.h"
#include <atomic>
#include <cassert>
#include <cstring>

using LuaBridgeUtil::VirtualKey;
using LuaBridgeUtil::WideAscii;
using InputUtil::KeyPostResult;
using InputUtil::PostKey;
using InputUtil::PostMouseButton;

struct Call
{
    std::string key;
    bool down{};
    bool result{};
};

HWND testWindow{};
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

int titleQueries{}, deliveredKeys{};
bool receiverKeys[256]{}, receivedShiftW{}, receivedCtrlShiftW{};
bool mtaAcceptedW{};

LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM key, LPARAM parameter)
{
    if(message == WM_GETTEXT || message == WM_GETTEXTLENGTH)
        ++titleQueries;
    if(message == WM_KEYDOWN || message == WM_KEYUP
        || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
    {
        ++deliveredKeys;
        assert(key < 256);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if(down && key == 'W' && !(parameter & (1u << 30))) mtaAcceptedW = true;
        receiverKeys[key] = down;
        if(down && key == 'W' && receiverKeys[VK_SHIFT])
        {
            receivedShiftW = true;
            if(receiverKeys[VK_CONTROL]) receivedCtrlShiftW = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, key, parameter);
}

HWND CreateProbeWindow(bool visible = false)
{
    WNDCLASSW type{};
    type.lpfnWndProc = ProbeWindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"DarkFlameInputProbe";
    assert(RegisterClassW(&type));
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        type.lpszClassName, L"DarkFlame input test", WS_POPUP | (visible ? WS_VISIBLE : 0),
        -32000, -32000, 1, 1, nullptr, nullptr, type.hInstance, nullptr);
    assert(window);
    return window;
}

void CheckWindowLookup()
{
    HWND window = CreateProbeWindow(true);
    titleQueries = 0;
    assert(InputUtil::GameWindow() == window);
    assert(titleQueries == 0);
    InputUtil::SetGameWindow(window);
    ShowWindow(window, SW_HIDE);
    assert(InputUtil::GameWindow() == window);
    HWND child = CreateWindowW(L"STATIC", L"Child", WS_CHILD, 0, 0, 1, 1,
        window, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(child);
    InputUtil::SetGameWindow(child);
    assert(InputUtil::GameWindow() == window);
    assert(titleQueries == 0);
    DestroyWindow(window);
    assert(InputUtil::OwnedRootWindow(window) == nullptr);
    assert(InputUtil::GameWindow() == nullptr);
    InputUtil::SetGameWindow(GetDesktopWindow());
    assert(InputUtil::GameWindow() == nullptr);
    std::puts("Window lookup does not invoke title handlers: OK");
}

void CheckFullQueue()
{
    HWND window = CreateProbeWindow();
    unsigned queued{};
    while(queued < 100000 && PostMessageW(window, WM_APP, 0, 0))
        ++queued;
    assert(queued > 0 && queued < 100000 && GetLastError() == ERROR_NOT_ENOUGH_QUOTA);
    deliveredKeys = 0;
    const auto result = PostKey(window, '2', true);
    assert(deliveredKeys == 0);
    assert(!result.posted && result.error == ERROR_NOT_ENOUGH_QUOTA);
    const auto shift = PostKey(window, VK_LSHIFT, true);
    assert(!shift.posted && shift.error == ERROR_NOT_ENOUGH_QUOTA);
    MSG message{};
    unsigned drained{};
    while(PeekMessageW(&message, window, WM_APP, WM_APP, PM_REMOVE))
        ++drained;
    assert(drained == queued);
    assert(PostMouseButton(window, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, true, 0));
    assert(PeekMessageW(&message, window, WM_RBUTTONDOWN, WM_RBUTTONDOWN, PM_REMOVE));
    assert(message.wParam == MK_RBUTTON);
    assert(PostMouseButton(window, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, false, 0));
    assert(PeekMessageW(&message, window, WM_RBUTTONUP, WM_RBUTTONUP, PM_REMOVE));
    assert(message.wParam == 0);
    assert(PostKey(window, '2', false).posted);
    assert(PeekMessageW(&message, window, WM_KEYUP, WM_KEYUP, PM_REMOVE));
    assert(deliveredKeys == 0);
    DispatchMessageW(&message);
    assert(deliveredKeys == 1);
    DestroyWindow(window);
    std::puts("Full queue fails without synchronous key dispatch; release recovers: OK");
}

void CheckKey(const char* name, bool down, UINT message, WPARAM key, UINT scan,
    bool extended = false, bool alt = false, bool repeat = false)
{
    Call call{name, down};
    assert(DirectEmulateKey(&call) == 1 && call.result);
    MSG actual{};
    assert(PeekMessageW(&actual, testWindow, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
    assert(actual.message == message && actual.wParam == key);
    assert((actual.lParam & 0xFFFF) == 1);
    assert(((actual.lParam >> 16) & 0xFF) == scan);
    assert(((actual.lParam >> 24) & 1) == extended);
    assert(((actual.lParam >> 29) & 1) == alt);
    assert(((actual.lParam >> 30) & 1) == (!down || repeat));
    assert(((actual.lParam >> 31) & 1) == !down);
    DispatchMessageW(&actual);
}

void CheckModifiers()
{
    testWindow = CreateProbeWindow();
    CheckKey("LSHIFT", true, WM_KEYDOWN, VK_SHIFT, 0x2A);
    CheckKey("W", true, WM_KEYDOWN, 'W', 0x11);
    assert(mtaAcceptedW);
    mtaAcceptedW = false; // Game cleared its control after focus/menu transition.
    CheckKey("W", true, WM_KEYDOWN, 'W', 0x11);
    assert(mtaAcceptedW);
    CheckKey("W", false, WM_KEYUP, 'W', 0x11);
    CheckKey("LSHIFT", false, WM_KEYUP, VK_SHIFT, 0x2A);
    assert(receivedShiftW && !receiverKeys[VK_SHIFT] && !receiverKeys['W']);
    CheckKey("RCTRL", true, WM_KEYDOWN, VK_CONTROL, 0x1D, true);
    CheckKey("RSHIFT", true, WM_KEYDOWN, VK_SHIFT, 0x36);
    CheckKey("W", true, WM_KEYDOWN, 'W', 0x11);
    CheckKey("W", false, WM_KEYUP, 'W', 0x11);
    Call mouse{"rmb", true};
    assert(DirectEmulateKey(&mouse) == 1 && mouse.result);
    ExpectMessage(WM_RBUTTONDOWN, MK_RBUTTON | MK_SHIFT | MK_CONTROL, false);
    mouse.down = false;
    assert(DirectEmulateKey(&mouse) == 1 && mouse.result);
    ExpectMessage(WM_RBUTTONUP, MK_SHIFT | MK_CONTROL, true);
    CheckKey("RSHIFT", false, WM_KEYUP, VK_SHIFT, 0x36);
    CheckKey("RCTRL", false, WM_KEYUP, VK_CONTROL, 0x1D, true);
    assert(receivedCtrlShiftW && !receiverKeys[VK_SHIFT] && !receiverKeys[VK_CONTROL]);
    for(const char* name : {"LALT", "RALT"})
    {
        const bool right = std::strcmp(name, "RALT") == 0;
        CheckKey(name, true, WM_SYSKEYDOWN, VK_MENU, 0x38, right, true);
        CheckKey("W", true, WM_SYSKEYDOWN, 'W', 0x11, false, true);
        CheckKey("W", false, WM_SYSKEYUP, 'W', 0x11, false, true);
        CheckKey(name, false, WM_SYSKEYUP, VK_MENU, 0x38, right);
    }
    CheckKey("TAB", true, WM_KEYDOWN, VK_TAB, 0x0F);
    CheckKey("TAB", false, WM_KEYUP, VK_TAB, 0x0F);
    CheckKey("ESC", true, WM_KEYDOWN, VK_ESCAPE, 0x01);
    CheckKey("ESC", false, WM_KEYUP, VK_ESCAPE, 0x01);
    CheckKey("UP", true, WM_KEYDOWN, VK_UP, 0x48, true);
    CheckKey("UP", false, WM_KEYUP, VK_UP, 0x48, true);
    CheckKey("F10", true, WM_SYSKEYDOWN, VK_F10, 0x44);
    CheckKey("F10", false, WM_SYSKEYUP, VK_F10, 0x44);
    for(const char* name : {"F0", "F25", "F1X", "RSHIFTT"})
    {
        Call invalid{name, true};
        assert(DirectEmulateKey(&invalid) == 1 && !invalid.result);
    }
    assert(VirtualKey("shift") == VK_LSHIFT && VirtualKey("ctrl") == VK_LCONTROL);
    assert(VirtualKey("F24") == VK_F24 && VirtualKey("backspace") == VK_BACK);
    MSG remaining{};
    assert(!PeekMessageW(&remaining, testWindow, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
    DestroyWindow(testWindow);
    std::puts("Left/right modifiers, Shift+W, Ctrl+Shift+W, Alt+W, modified RMB and special keys: OK");
}

int main(int argc, char** argv)
{
    if(argc > 1)
    {
        if(std::strcmp(argv[1], "lookup") == 0) CheckWindowLookup();
        else if(std::strcmp(argv[1], "full-queue") == 0) CheckFullQueue();
        else if(std::strcmp(argv[1], "modifiers") == 0) CheckModifiers();
        else return 1;
        return 0;
    }
    testWindow = CreateWindowW(L"STATIC", L"Input test", 0, 0, 0, 1, 1,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(testWindow);
    for(const char* key : {"w", "s", "a", "d", "2", "K", "L", "SPACE", "ENTER", "LSHIFT"})
    {
        for(bool down : {true, false})
        {
            Call call{key, down};
            assert(DirectEmulateKey(&call) == 1 && call.result);
            ExpectMessage(down ? WM_KEYDOWN : WM_KEYUP, InputUtil::MessageKey(VirtualKey(key)), !down);
        }
    }
    for(const char* button : {"rmb", "right", "lmb", "left"})
    {
        const bool right = std::string(button) == "rmb" || std::string(button) == "right";
        for(bool down : {true, false})
        {
            Call call{button, down};
            const int result = (std::string(button) == "rmb" || std::string(button) == "lmb")
                ? DirectEmulateKey(&call) : DirectEmulateMouseButton(&call);
            assert(result == 1 && call.result);
            ExpectMessage(right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP), down ? (right ? MK_RBUTTON : MK_LBUTTON) : 0, !down);
        }
    }
    Call invalid{"unsupported", true};
    DirectEmulateKey(&invalid);
    assert(!invalid.result);
    for(int stop = 0; stop < 2000; ++stop)
    {
        for(bool down : {true, false})
        {
            Call doors{"2", down};
            assert(DirectEmulateKey(&doors) == 1 && doors.result);
            ExpectMessage(down ? WM_KEYDOWN : WM_KEYUP, '2', !down);
            Call mouse{"rmb", down};
            assert(DirectEmulateKey(&mouse) == 1 && mouse.result);
            ExpectMessage(down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, !down);
        }
    }
    DestroyWindow(testWindow);
    testWindow = nullptr;
    Call mouse{"rmb", true};
    DirectEmulateKey(&mouse);
    assert(!mouse.result);
    Call key{"w", true};
    DirectEmulateKey(&key);
    assert(!key.result);
    std::puts("PostMessage keyboard, RMB alias, original mouse API, 2000 door/RMB cycles: OK");
}
