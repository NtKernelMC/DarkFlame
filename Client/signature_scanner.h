#pragma once

#include <Windows.h>

#include <cstdint>
#include <string_view>
#include <vector>

class SignatureScanner
{
public:
    explicit SignatureScanner(HMODULE module);
    std::uintptr_t Find(std::string_view idaPattern) const;

private:
    struct MemoryRange
    {
        BYTE* base{};
        std::size_t size{};
    };

    struct PatternByte
    {
        BYTE value{};
        bool wildcard{};
    };

    static std::vector<PatternByte> Parse(std::string_view pattern);

    std::vector<MemoryRange> m_ranges;
};
