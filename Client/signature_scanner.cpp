#include "signature_scanner.h"

#include <algorithm>
#include <cctype>

namespace
{
int HexValue(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}
}

SignatureScanner::SignatureScanner(HMODULE module)
{
    if (!module)
        return;

    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    const auto imageSize = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section)
    {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            || section->VirtualAddress >= imageSize)
            continue;

        const std::size_t sectionSize = std::max<std::size_t>(
            section->Misc.VirtualSize, section->SizeOfRawData);
        const std::size_t available = imageSize - section->VirtualAddress;
        m_ranges.push_back({base + section->VirtualAddress, std::min(sectionSize, available)});
    }
}

std::vector<SignatureScanner::PatternByte> SignatureScanner::Parse(std::string_view pattern)
{
    std::vector<PatternByte> result;
    for (std::size_t index = 0; index < pattern.size();)
    {
        while (index < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[index])))
            ++index;
        if (index >= pattern.size())
            break;

        if (pattern[index] == '?')
        {
            result.push_back({0, true});
            ++index;
            if (index < pattern.size() && pattern[index] == '?')
                ++index;
            continue;
        }

        if (index + 1 >= pattern.size())
            return {};
        const int high = HexValue(pattern[index]);
        const int low = HexValue(pattern[index + 1]);
        if (high < 0 || low < 0)
            return {};
        result.push_back({static_cast<BYTE>((high << 4) | low), false});
        index += 2;
    }
    return result;
}

std::uintptr_t SignatureScanner::Find(std::string_view idaPattern) const
{
    const auto pattern = Parse(idaPattern);
    if (pattern.empty())
        return 0;

    for (const MemoryRange& range : m_ranges)
    {
        if (!range.base || pattern.size() > range.size)
            continue;
        const std::size_t last = range.size - pattern.size();
        for (std::size_t offset = 0; offset <= last; ++offset)
        {
            bool matches = true;
            for (std::size_t index = 0; index < pattern.size(); ++index)
            {
                if (!pattern[index].wildcard && range.base[offset + index] != pattern[index].value)
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
                return reinterpret_cast<std::uintptr_t>(range.base + offset);
        }
    }
    return 0;
}
