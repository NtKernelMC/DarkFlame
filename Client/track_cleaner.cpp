#include "track_cleaner.h"

#include "logger.h"

#include <Windows.h>
#include <wincred.h>

#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>

namespace
{
bool DeleteCredentialIfPresent(const wchar_t* name)
{
    if(CredDeleteW(name, CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

bool DeleteFileIfPresent(const wchar_t* path)
{
    if(DeleteFileW(path)) return true;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool DeleteDirectoryIfPresent(const wchar_t* path)
{
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
}

bool DeleteRegistryTreeIfPresent(HKEY root, const wchar_t* path)
{
    const LONG status = RegDeleteTreeW(root, path);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND
        || status == ERROR_PATH_NOT_FOUND;
}

bool WriteRegistryText(const wchar_t* path, const wchar_t* name,
    const wchar_t* value)
{
    HKEY key{};
    const LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0,
        KEY_SET_VALUE | KEY_WOW64_32KEY, &key);
    if(opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND)
        return true;
    if(opened != ERROR_SUCCESS) return false;

    const DWORD bytes = static_cast<DWORD>((std::wcslen(value) + 1)
        * sizeof(wchar_t));
    const LONG written = RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

bool ClearInstallState(const wchar_t* path)
{
    return WriteRegistryText(path, L"Last Run Path Hash", L"");
}

bool ClearGeneralState(const wchar_t* path)
{
    const bool checksum = WriteRegistryText(path, L"cachechecksum", L"");
    return WriteRegistryText(path, L"serial", L"") && checksum;
}

bool ClearDiagnostics(const wchar_t* path)
{
    bool clean = WriteRegistryText(path, L"crash-data", L"");
    clean = WriteRegistryText(path, L"crash-data1", L"") && clean;
    clean = WriteRegistryText(path, L"crash-data2", L"") && clean;
    return WriteRegistryText(path, L"send-dumps", L"no") && clean;
}
}

bool CleanTracks()
{
    constexpr std::array cachePaths{
        L"C:\\ProgramData\\MTA San Andreas All\\Common\\data\\cache",
        L"C:\\ProgramData\\RocketMTA\\Common\\data\\cache",
        L"C:\\ProgramData\\NEXTRP All\\Common\\data\\cache",
        L"C:\\ProgramData\\RAZEGTA All\\Common\\data\\cache"
    };
    constexpr std::array installKeys{
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: Province All\\1.6",
        L"SOFTWARE\\WOW6432Node\\NEXTRP All\\1.6",
        L"SOFTWARE\\WOW6432Node\\ODU RP\\1.5",
        L"SOFTWARE\\WOW6432Node\\RocketMTA\\1.6",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: PRVRemake All\\1.5",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: San Andreas All\\1.5",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: San Andreas All\\1.6",
        L"SOFTWARE\\WOW6432Node\\RAZEGTA: All\\1.6",
        L"SOFTWARE\\WOW6432Node\\UKRAINEGTA: GLAB3\\1.5",
        L"SOFTWARE\\WOW6432Node\\UKRAINEGTA: GLAB3\\1.6"
    };
    constexpr std::array generalKeys{
        L"SOFTWARE\\WOW6432Node\\ODU RP\\1.5\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\NEXTRP All\\1.6\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: Province All\\1.6\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\RocketMTA\\1.6\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: PRVRemake All\\1.6\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\RAZEGTA: All\\1.6\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: San Andreas All\\1.5\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: San Andreas All\\1.6\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\UKRAINEGTA: GLAB3\\1.5\\Settings\\general",
        L"SOFTWARE\\WOW6432Node\\UKRAINEGTA: GLAB3\\1.6\\Settings\\general"
    };
    constexpr std::array diagnosticKeys{
        L"SOFTWARE\\WOW6432Node\\NEXTRP All\\1.6\\Settings\\diagnostics",
        L"SOFTWARE\\WOW6432Node\\ODU RP\\1.5\\Settings\\diagnostics",
        L"SOFTWARE\\WOW6432Node\\RAZEGTA: All\\1.6\\Settings\\diagnostics",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: PRVRemake All\\1.6\\Settings\\diagnostics",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: San Andreas All\\1.6\\Settings\\diagnostics",
        L"SOFTWARE\\WOW6432Node\\Multi Theft Auto: Province All\\1.6\\Settings\\diagnostics",
        L"SOFTWARE\\WOW6432Node\\RocketMTA\\1.6\\Settings\\diagnostics"
    };

    bool clean = DeleteCredentialIfPresent(L"SSO_RND_DEVICE");
    clean = DeleteFileIfPresent(L"C:\\ProgramData:NT") && clean;
    clean = DeleteFileIfPresent(L"C:\\ProgramData:NT2") && clean;
    for(const wchar_t* path : cachePaths)
        clean = DeleteDirectoryIfPresent(path) && clean;

    std::array<wchar_t, 32768> appData{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData.data(),
        static_cast<DWORD>(appData.size()));
    if(length && length < appData.size())
    {
        const std::wstring roaming(appData.data(), length);
        clean = DeleteFileIfPresent((roaming + L":NT").c_str()) && clean;
        clean = DeleteFileIfPresent((roaming + L":NT2").c_str()) && clean;
    }
    else
        clean = false;

    clean = DeleteRegistryTreeIfPresent(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CLSID") && clean;
    clean = DeleteRegistryTreeIfPresent(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CLSID2") && clean;
    clean = DeleteRegistryTreeIfPresent(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Connections") && clean;
    for(const wchar_t* path : installKeys)
        clean = ClearInstallState(path) && clean;
    for(const wchar_t* path : generalKeys)
        clean = ClearGeneralState(path) && clean;
    for(const wchar_t* path : diagnosticKeys)
        clean = ClearDiagnostics(path) && clean;

    Log::Write(clean ? L"[serial] tracks cleaned before disk identity hook"
        : L"[serial] track cleanup completed with partial failures");
    return clean;
}
