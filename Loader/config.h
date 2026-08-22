#pragma once

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

struct DarkFlameConfig
{
    bool antiShadow{true};
    bool setSerial{};
    bool randomSerial{};
    bool serialValid{true};
    std::string publicSerial{"9F5A1A5F9008ED9327D64B4A700324F3"};
};

namespace Config
{
inline constexpr std::string_view Defaults =
    "ANTI_SHADOW=1\n"
    "SET_SERIAL=0\n"
    "RANDOM_SERIAL=0\n"
    "PUBLIC_SERIAL=9F5A1A5F9008ED9327D64B4A700324F3\n";

inline std::string Trim(std::string value)
{
    const auto space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    return value;
}

inline void Upper(std::string& value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::toupper(ch));
    });
}

inline bool ValidSerial(std::string_view value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char ch)
    {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
    });
}

inline DarkFlameConfig Load(const std::wstring& directory)
{
    DarkFlameConfig config;
    const std::filesystem::path path = std::filesystem::path(directory) / L"DarkFlame.cfg";
    if(!std::filesystem::exists(path))
    {
        std::ofstream created(path, std::ios::binary);
        created.write(Defaults.data(), static_cast<std::streamsize>(Defaults.size()));
    }

    std::ifstream file(path, std::ios::binary);
    std::string line;
    while(std::getline(file, line))
    {
        line = Trim(std::move(line));
        if(line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const std::size_t equals = line.find('=');
        if(equals == std::string::npos)
            continue;
        std::string key = Trim(line.substr(0, equals));
        std::string value = Trim(line.substr(equals + 1));
        Upper(key);
        if(key == "ANTI_SHADOW")
            config.antiShadow = value != "0";
        else if(key == "SET_SERIAL")
            config.setSerial = value == "1";
        else if(key == "RANDOM_SERIAL")
            config.randomSerial = value == "1";
        else if(key == "PUBLIC_SERIAL")
        {
            Upper(value);
            config.publicSerial = std::move(value);
        }
    }
    if(!ValidSerial(config.publicSerial))
    {
        config.publicSerial = "9F5A1A5F9008ED9327D64B4A700324F3";
        config.serialValid = false;
    }
    if(config.setSerial && config.randomSerial)
        config.randomSerial = false;
    return config;
}

inline bool Save(const std::wstring& directory, const DarkFlameConfig& config)
{
    const std::filesystem::path path = std::filesystem::path(directory) / L"DarkFlame.cfg";
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    std::string text = "ANTI_SHADOW=" + std::string(config.antiShadow ? "1\n" : "0\n")
        + "SET_SERIAL=" + std::string(config.setSerial ? "1\n" : "0\n")
        + "RANDOM_SERIAL=" + std::string(config.randomSerial ? "1\n" : "0\n")
        + "PUBLIC_SERIAL=" + config.publicSerial + "\n";

    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written{};
    const bool saved = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
        &written, nullptr) && written == static_cast<DWORD>(text.size())
        && FlushFileBuffers(file);
    CloseHandle(file);
    if(!saved || !MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
}
