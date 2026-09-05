#pragma once

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ScriptDumperUtil
{
inline std::wstring Utf8ToWide(std::string_view text)
{
    if(text.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if(!size)
    {
        codePage = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(codePage, flags, text.data(),
            static_cast<int>(text.size()), nullptr, 0);
    }
    if(!size)
        return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if(!MultiByteToWideChar(codePage, flags, text.data(),
        static_cast<int>(text.size()), output.data(), size))
    {
        return {};
    }
    return output;
}

inline std::wstring ErrorText(std::string_view text)
{
    std::wstring result = Utf8ToWide(text);
    if(result.empty() && !text.empty())
        result = L"unknown error";
    return result;
}

inline bool IsReservedDeviceName(std::string value)
{
    const std::size_t dot = value.find('.');
    if(dot != std::string::npos)
        value.resize(dot);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if(value == "CON" || value == "PRN" || value == "AUX" || value == "NUL")
        return true;
    if(value.size() != 4)
        return false;
    return (value.starts_with("COM") || value.starts_with("LPT"))
        && value[3] >= '1' && value[3] <= '9';
}

inline bool SanitizeComponent(std::string_view input, std::string& output)
{
    if(input.empty() || input == "." || input == ".."
        || input.find('\0') != std::string_view::npos)
    {
        return false;
    }
    output.clear();
    output.reserve(input.size() + 1);
    for(unsigned char ch : input)
    {
        if(ch < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"'
            || ch == '<' || ch == '>' || ch == '|')
        {
            continue;
        }
        output.push_back(static_cast<char>(ch));
    }
    while(!output.empty() && (output.back() == ' ' || output.back() == '.'))
        output.pop_back();
    if(output.empty() || output == "." || output == "..")
        return false;
    if(IsReservedDeviceName(output))
        output.insert(output.begin(), '_');
    return true;
}

inline bool SplitPacketPath(std::string path,
    std::vector<std::string>& components)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if(path.empty() || path.front() == '/')
        return false;
    std::size_t offset{};
    while(offset <= path.size())
    {
        const std::size_t separator = path.find('/', offset);
        const std::size_t end = separator == std::string::npos
            ? path.size() : separator;
        std::string component;
        if(!SanitizeComponent(std::string_view(path).substr(offset,
            end - offset), component))
        {
            return false;
        }
        components.push_back(std::move(component));
        if(separator == std::string::npos)
            break;
        offset = separator + 1;
    }
    return !components.empty();
}

inline bool EqualAsciiInsensitive(std::string_view left,
    std::string_view right)
{
    if(left.size() != right.size())
        return false;
    for(std::size_t index{}; index < left.size(); ++index)
    {
        if(std::tolower(static_cast<unsigned char>(left[index]))
            != std::tolower(static_cast<unsigned char>(right[index])))
        {
            return false;
        }
    }
    return true;
}

inline bool AtomicWrite(const std::filesystem::path& path,
    std::span<const std::uint8_t> data, std::uint64_t sequence)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if(error)
        return false;

    std::filesystem::path temporary = path;
    temporary += L".darkflame-" + std::to_wstring(sequence) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
        return false;

    bool written = true;
    std::size_t offset{};
    while(offset < data.size())
    {
        const DWORD request = static_cast<DWORD>((std::min)(data.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD count{};
        if(!WriteFile(file, data.data() + offset, request, &count, nullptr)
            || count != request)
        {
            written = false;
            break;
        }
        offset += count;
    }
    written = written && FlushFileBuffers(file);
    CloseHandle(file);
    if(written)
    {
        written = MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if(!written)
        DeleteFileW(temporary.c_str());
    return written;
}

inline bool AtomicWrite(const std::filesystem::path& path,
    std::string_view text, std::uint64_t sequence)
{
    return AtomicWrite(path,
        std::span(reinterpret_cast<const std::uint8_t*>(text.data()),
            text.size()), sequence);
}

inline std::filesystem::path RawBytecodePath(
    const std::filesystem::path& luaPath)
{
    std::filesystem::path raw = luaPath;
    raw.replace_extension(L".luac");
    return raw;
}

inline std::filesystem::path LuaOutputPath(
    const std::filesystem::path& input)
{
    std::filesystem::path output = input;
    output.replace_extension(L".lua");
    return output;
}

class PacketReader
{
public:
    explicit PacketReader(std::span<const std::uint8_t> data) : m_data(data) {}

    template<class T>
    bool Read(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if(sizeof(T) > Remaining())
            return false;
        std::memcpy(&value, m_data.data() + m_offset, sizeof(T));
        m_offset += sizeof(T);
        return true;
    }

    bool Read(std::string& value, std::size_t size)
    {
        if(size > Remaining())
            return false;
        value.assign(reinterpret_cast<const char*>(m_data.data() + m_offset),
            size);
        m_offset += size;
        return true;
    }

    bool Read(std::vector<std::uint8_t>& value, std::size_t size)
    {
        if(size > Remaining())
            return false;
        value.assign(m_data.begin() + m_offset, m_data.begin() + m_offset + size);
        m_offset += size;
        return true;
    }

    std::size_t Remaining() const { return m_data.size() - m_offset; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_offset{};
};
}
