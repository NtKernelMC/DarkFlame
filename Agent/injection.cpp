#include "injection.h"

#include "../Shared/manual_map.h"

bool MapLibrary(HANDLE process, const std::wstring& path,
    bool* exceptionSupport)
{
    return ManualMap::Map(process, path, nullptr, 0, exceptionSupport);
}
