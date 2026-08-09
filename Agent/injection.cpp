#include "injection.h"

#include "../Shared/manual_map.h"

bool MapLibrary(HANDLE process, const std::wstring& path)
{
    return ManualMap::Map(process, path);
}
