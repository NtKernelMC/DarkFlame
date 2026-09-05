#pragma once

#include <Windows.h>

namespace InputUtil
{
struct KeyPostResult
{
    bool posted{};
    DWORD error{ERROR_INVALID_WINDOW_HANDLE};
    UINT scan{};
};

inline HWND ProcessWindow()
{
    DWORD process{};
    HWND window = GetForegroundWindow();
    if(window)
        GetWindowThreadProcessId(window, &process);
    if(process == GetCurrentProcessId())
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
            || !IsWindowVisible(candidate) || !GetWindowTextLengthW(candidate))
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
    HWND window = FindWindowA(nullptr, "MTA: Province");
    if(!window) window = FindWindowA(nullptr, "MTA: San Andreas");
    if(!window) window = ProcessWindow();
    if(!window)
        return nullptr;

    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if(process != GetCurrentProcessId())
        window = ProcessWindow();
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}

inline KeyPostResult PostKey(HWND window, int virtualKey, bool pressed)
{
    KeyPostResult result;
    const DWORD windowThread = window
        ? GetWindowThreadProcessId(window, nullptr) : GetCurrentThreadId();
    const HKL layout = GetKeyboardLayout(windowThread);
    result.scan = MapVirtualKeyExW(static_cast<UINT>(virtualKey),
        MAPVK_VK_TO_VSC_EX, layout);
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
        parameter |= static_cast<LPARAM>((1u << 30) | (1u << 31));
    const UINT message = virtualKey == VK_LMENU
        ? (pressed ? WM_SYSKEYDOWN : WM_SYSKEYUP)
        : (pressed ? WM_KEYDOWN : WM_KEYUP);
    if(message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
        parameter |= static_cast<LPARAM>(1u << 29);
    if(!window || !IsWindow(window))
        return result;

    SetLastError(ERROR_SUCCESS);
    result.posted = PostMessageW(window, message, virtualKey, parameter) != FALSE;
    result.error = result.posted ? ERROR_SUCCESS : GetLastError();
    if(result.posted)
        return result;

    DWORD_PTR ignored{};
    SetLastError(ERROR_SUCCESS);
    result.posted = SendMessageTimeoutW(window, message, virtualKey, parameter,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &ignored) != 0;
    result.error = result.posted ? ERROR_SUCCESS : GetLastError();
    return result;
}
}
