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
        const SHORT code = VkKeyScanA(key.front());
        return code == -1 ? 0 : LOBYTE(code);
    }
    if(key == "SPACE") return VK_SPACE;
    if(key == "ENTER") return VK_RETURN;
    if(key == "LSHIFT") return VK_LSHIFT;
    if(key == "LCTRL") return VK_LCONTROL;
    if(key == "LALT") return VK_LMENU;
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
