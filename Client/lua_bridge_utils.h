#pragma once

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace LuaBridgeUtil
{
inline std::wstring WideAscii(std::string_view text)
{
    return {text.begin(), text.end()};
}

inline int VirtualKey(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value)
    {
        return static_cast<char>(std::toupper(value));
    });
    if(key.size() == 1)
    {
        const unsigned char value = static_cast<unsigned char>(key.front());
        if((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))
            return value;
        switch(value)
        {
        case '[': return VK_OEM_4;
        case ']': return VK_OEM_6;
        case ';': return VK_OEM_1;
        case '\'': return VK_OEM_7;
        case ',': return VK_OEM_COMMA;
        case '.': return VK_OEM_PERIOD;
        case '/': return VK_OEM_2;
        case '\\': return VK_OEM_5;
        case '`': return VK_OEM_3;
        case '-': return VK_OEM_MINUS;
        case '=': return VK_OEM_PLUS;
        }
        const SHORT code = VkKeyScanA(key.front());
        return code == -1 ? 0 : LOBYTE(code);
    }
    if(key == "SPACE") return VK_SPACE;
    if(key == "ENTER" || key == "RETURN") return VK_RETURN;
    if(key == "SHIFT" || key == "LSHIFT") return VK_LSHIFT;
    if(key == "RSHIFT") return VK_RSHIFT;
    if(key == "CTRL" || key == "CONTROL" || key == "LCTRL" || key == "LCONTROL") return VK_LCONTROL;
    if(key == "RCTRL" || key == "RCONTROL") return VK_RCONTROL;
    if(key == "ALT" || key == "LALT") return VK_LMENU;
    if(key == "RALT") return VK_RMENU;
    if(key == "TAB") return VK_TAB;
    if(key == "ESC" || key == "ESCAPE") return VK_ESCAPE;
    if(key == "BACKSPACE" || key == "BACK") return VK_BACK;
    if(key == "CAPSLOCK") return VK_CAPITAL;
    if(key == "NUMLOCK") return VK_NUMLOCK;
    if(key == "SCROLLLOCK") return VK_SCROLL;
    if(key == "INSERT" || key == "INS") return VK_INSERT;
    if(key == "DELETE" || key == "DEL") return VK_DELETE;
    if(key == "HOME") return VK_HOME;
    if(key == "END") return VK_END;
    if(key == "PAGEUP" || key == "PGUP") return VK_PRIOR;
    if(key == "PAGEDOWN" || key == "PGDN") return VK_NEXT;
    if(key == "UP" || key == "ARROW_U") return VK_UP;
    if(key == "DOWN" || key == "ARROW_D") return VK_DOWN;
    if(key == "LEFT" || key == "ARROW_L") return VK_LEFT;
    if(key == "RIGHT" || key == "ARROW_R") return VK_RIGHT;
    if(key.size() >= 2 && key.size() <= 3 && key[0] == 'F'
        && key[1] >= '1' && key[1] <= '9')
    {
        int number = key[1] - '0';
        if(key.size() == 3)
        {
            if(key[2] < '0' || key[2] > '9') return 0;
            number = number * 10 + key[2] - '0';
        }
        if(number <= 24) return VK_F1 + number - 1;
    }
    return 0;
}

inline std::string LuaLiteral(std::string_view value)
{
    std::string output{"\""};
    output.reserve(value.size() + 16);
    for(const unsigned char character : value)
    {
        switch(character)
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if(character >= 0x20 && character < 0x7F)
                output.push_back(static_cast<char>(character));
            else
            {
                char escaped[5]{};
                std::snprintf(escaped, sizeof(escaped), "\\%03u", character);
                output += escaped;
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

inline std::string ThreadKey(std::uintptr_t id)
{
    char key[24]{};
    std::snprintf(key, sizeof(key), "0x%08lX", static_cast<unsigned long>(id));
    return key;
}

inline std::string Timestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char output[24]{};
    std::snprintf(output, sizeof(output), "%02u:%02u:%02u.%03u",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return output;
}

inline std::string Escape(std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for(const unsigned char character : value)
    {
        switch(character)
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\r': output += "\\r"; break;
        case '\n': output += "\\n"; break;
        case '\t': output += "\\t"; break;
        default:
            if(character >= 0x20)
                output.push_back(static_cast<char>(character));
            else
            {
                char escaped[5]{};
                std::snprintf(escaped, sizeof(escaped), "\\x%02X", character);
                output += escaped;
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

inline std::string Hex(std::uintptr_t value)
{
    char output[24]{};
    std::snprintf(output, sizeof(output), "0x%08lX",
        static_cast<unsigned long>(value));
    return output;
}
}
