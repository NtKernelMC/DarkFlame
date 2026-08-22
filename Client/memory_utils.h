#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace MemoryUtil
{
inline bool Readable(const void* pointer, std::size_t size)
{
    if(!pointer || !size)
        return pointer && !size;
    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(pointer);
    if(cursor > std::numeric_limits<std::uintptr_t>::max() - size)
        return false;
    const std::uintptr_t finish = cursor + size;
    while(cursor < finish)
    {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
            sizeof(info)) || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        {
            return false;
        }
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(
            info.BaseAddress);
        if(base > std::numeric_limits<std::uintptr_t>::max() - info.RegionSize)
            return false;
        const std::uintptr_t end = base + info.RegionSize;
        if(end <= cursor)
            return false;
        cursor = (std::min)(end, finish);
    }
    return true;
}

inline bool Writable(const void* pointer, std::size_t size)
{
    if(!Readable(pointer, size))
        return false;
    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(pointer);
    const std::uintptr_t finish = cursor + size;
    while(cursor < finish)
    {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
            sizeof(info)))
        {
            return false;
        }
        const DWORD protection = info.Protect & 0xFF;
        if(protection != PAGE_READWRITE && protection != PAGE_WRITECOPY
            && protection != PAGE_EXECUTE_READWRITE
            && protection != PAGE_EXECUTE_WRITECOPY)
        {
            return false;
        }
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(
            info.BaseAddress) + static_cast<std::uintptr_t>(info.RegionSize);
        cursor = (std::min)(finish, end);
    }
    return true;
}

template<class T>
bool ReadValue(const void* base, std::size_t offset, T& value)
{
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(base);
    if(address > std::numeric_limits<std::uintptr_t>::max() - offset)
        return false;
    const void* source = reinterpret_cast<const void*>(address + offset);
    if(!Readable(source, sizeof(T)))
        return false;
    std::memcpy(&value, source, sizeof(T));
    return true;
}

inline bool ReadSString(const void* object, std::string& output,
    std::size_t limit)
{
    std::uint32_t length{};
    std::uint32_t capacity{};
    if(!ReadValue(object, 16, length) || !ReadValue(object, 20, capacity)
        || length > capacity || length > 1024 * 1024)
    {
        return false;
    }
    const char* text{};
    if(capacity <= 15)
        text = static_cast<const char*>(object);
    else if(!ReadValue(object, 0, text))
        return false;
    const std::size_t copied = (std::min)(limit,
        static_cast<std::size_t>(length));
    if(copied && !Readable(text, copied))
        return false;
    output.assign(text ? text : "", copied);
    return true;
}
}
