#pragma once

#include <Windows.h>
#include <atomic>
#include "win32_sync.h"

namespace InputUtil
{
inline std::atomic<HWND> g_gameWindow{};

inline HWND OwnedRootWindow(HWND window)
{
    DWORD process{};
    if(!window || !GetWindowThreadProcessId(window, &process)
        || process != GetCurrentProcessId())
        return nullptr;
    return GetAncestor(window, GA_ROOT);
}

inline void SetGameWindow(HWND window)
{
    g_gameWindow.store(OwnedRootWindow(window), std::memory_order_release);
}

struct KeyPostResult
{
    bool posted{};
    DWORD error{ERROR_INVALID_WINDOW_HANDLE};
    UINT scan{};
};

struct PostedInputState
{
    HWND window{};
    DWORD thread{};
    bool keys[256]{};
    UINT scans[256]{};
    WPARAM buttons{};
};

inline PostedInputState g_postedInput;
inline SRWLOCK g_postedInputLock = SRWLOCK_INIT;

inline PostedInputState& PostedInput(HWND window, DWORD thread)
{
    auto& state = g_postedInput;
    if(state.window != window || state.thread != thread)
        state = {window, thread};
    return state;
}

inline int MessageKey(int virtualKey)
{
    if(virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT) return VK_SHIFT;
    if(virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL) return VK_CONTROL;
    if(virtualKey == VK_LMENU || virtualKey == VK_RMENU) return VK_MENU;
    return virtualKey;
}

inline HWND ProcessWindow()
{
    HWND window = OwnedRootWindow(GetForegroundWindow());
    if(window)
        return window;

    struct Search
    {
        DWORD process;
        HWND window;
    } search{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND candidate, LPARAM parameter) -> BOOL
    {
        auto& search = *reinterpret_cast<Search*>(parameter);
        DWORD process{};
        GetWindowThreadProcessId(candidate, &process);
        if(process != search.process || GetWindow(candidate, GW_OWNER)
            || !IsWindowVisible(candidate))
        {
            return TRUE;
        }
        search.window = candidate;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

inline HWND GameWindow()
{
    HWND window = OwnedRootWindow(g_gameWindow.load(std::memory_order_acquire));
    return window ? window : ProcessWindow();
}

inline KeyPostResult PostKey(HWND window, int virtualKey, bool pressed)
{
    KeyPostResult result;
    DWORD process{};
    const DWORD windowThread = window ? GetWindowThreadProcessId(window, &process) : 0;
    if(!windowThread || process != GetCurrentProcessId())
        return result;
    if(virtualKey <= 0 || virtualKey >= 256)
    {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    if(virtualKey == VK_SHIFT) virtualKey = VK_LSHIFT;
    if(virtualKey == VK_CONTROL) virtualKey = VK_LCONTROL;
    if(virtualKey == VK_MENU) virtualKey = VK_LMENU;
    Win32Sync::ExclusiveLock lock(g_postedInputLock);
    auto& state = PostedInput(window, windowThread);
    const HKL layout = GetKeyboardLayout(windowThread);
    result.scan = state.keys[virtualKey] ? state.scans[virtualKey]
        : MapVirtualKeyExW(static_cast<UINT>(virtualKey),
        MAPVK_VK_TO_VSC_EX, layout);
    if(!result.scan)
    {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    LPARAM parameter = 1 | static_cast<LPARAM>((result.scan & 0xFF) << 16);
    const bool extended = (result.scan & 0xFF00) == 0xE000
        || virtualKey == VK_RMENU || virtualKey == VK_RCONTROL
        || virtualKey == VK_NUMLOCK || virtualKey == VK_INSERT
        || virtualKey == VK_DELETE || virtualKey == VK_HOME
        || virtualKey == VK_END || virtualKey == VK_PRIOR
        || virtualKey == VK_NEXT || virtualKey == VK_UP
        || virtualKey == VK_DOWN || virtualKey == VK_LEFT
        || virtualKey == VK_RIGHT || virtualKey == VK_DIVIDE;
    if(extended)
        parameter |= static_cast<LPARAM>(1u << 24);
    if(!pressed)
        parameter |= static_cast<LPARAM>(1u << 30);
    if(!pressed)
        parameter |= static_cast<LPARAM>(1u << 31);
    const int messageKey = MessageKey(virtualKey);
    const bool altDown = state.keys[VK_LMENU] || state.keys[VK_RMENU];
    const bool altContext = messageKey == VK_MENU
        ? pressed || state.keys[virtualKey == VK_LMENU ? VK_RMENU : VK_LMENU] : altDown;
    const UINT message = messageKey == VK_MENU || altDown || virtualKey == VK_F10
        ? (pressed ? WM_SYSKEYDOWN : WM_SYSKEYUP)
        : (pressed ? WM_KEYDOWN : WM_KEYUP);
    if(altContext)
        parameter |= static_cast<LPARAM>(1u << 29);

    SetLastError(ERROR_SUCCESS);
    result.posted = PostMessageA(window, message, messageKey, parameter) != FALSE;
    result.error = result.posted ? ERROR_SUCCESS : GetLastError();
    if(result.posted)
    {
        state.keys[virtualKey] = pressed;
        state.scans[virtualKey] = result.scan;
    }
    return result;
}

inline bool PostMouseButton(HWND window, UINT down, UINT up, WPARAM button, bool pressed, LPARAM position)
{
    DWORD process{};
    const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    if(!thread || process != GetCurrentProcessId())
        return false;
    Win32Sync::ExclusiveLock lock(g_postedInputLock);
    auto& state = PostedInput(window, thread);
    const WPARAM buttons = pressed ? state.buttons | button : state.buttons & ~button;
    WPARAM modifiers{};
    if(state.keys[VK_LSHIFT] || state.keys[VK_RSHIFT]) modifiers |= MK_SHIFT;
    if(state.keys[VK_LCONTROL] || state.keys[VK_RCONTROL]) modifiers |= MK_CONTROL;
    if(!PostMessageA(window, pressed ? down : up, buttons | modifiers, position))
        return false;
    state.buttons = buttons;
    return true;
}
}
