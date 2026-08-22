#include "privacy_hooks.h"

#include "logger.h"

#include <MinHook.h>
#include <Wbemidl.h>
#include <iphlpapi.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr ULONGLONG FileTimeTicksPerDay = 864000000000ULL;
constexpr ULONGLONG EdgeMinimumAge = FileTimeTicksPerDay * 365 * 2;
constexpr ULONGLONG EdgeMaximumAge = FileTimeTicksPerDay * 365 * 5;

using GetAdaptersInfoFn = ULONG(WINAPI*)(PIP_ADAPTER_INFO, PULONG);
using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD,
    LPBYTE, LPDWORD);
using RegGetValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD,
    PVOID, LPDWORD);
using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
using WmiGetFn = HRESULT(WINAPI*)(void*, LPCWSTR, long, VARIANT*, CIMTYPE*, long*);

struct PrivacyProfile
{
    std::wstring hwProfileGuid;
    std::array<BYTE, 20> clientHwid{};
    std::wstring machineId;
    std::wstring productId;
    std::array<BYTE, 24> mountedDevice{};
    std::wstring machineGuid;
    std::wstring systemUuid;
    std::wstring processorName;
    std::wstring processorVendor;
    std::wstring processorIdentifier;
    std::wstring processorId;
    std::array<BYTE, 4> processorRevision{};
    std::wstring baseBoardManufacturer;
    std::wstring baseBoardProduct;
    std::wstring baseBoardSerial;
    std::wstring biosSerial;
    std::wstring biosReleaseDate;
    std::wstring biosWmiReleaseDate;
    std::wstring biosVendor;
    std::wstring biosVersion;
    std::wstring autoLogonSid;
    ULONGLONG edgeInstallTime{};
    std::wstring primaryAdapter;
    std::array<BYTE, 6> mac{};
};

struct ExportHook
{
    const wchar_t* module{};
    const char* name{};
    void* detour{};
    void** original{};
    void* target{};
    std::array<BYTE, 8> patch{};
    bool installed{};
};

struct FakeValue
{
    DWORD type{};
    const void* data{};
    DWORD size{};
};

enum class HardwarePlatform
{
    IntelLga1150,
    IntelLga1151Legacy,
    IntelLga1151Coffee,
    IntelLga1200,
    IntelLga1700,
    IntelMobile,
    AmdLegacyAm4,
    AmdModernAm4,
    AmdMobile,
    ThreadripperX399,
    ThreadripperTrx40
};

struct BoardModel
{
    std::wstring_view manufacturer;
    std::wstring_view product;
    std::wstring_view biosVendor;
};

std::atomic_bool g_started{};
std::atomic_bool g_installed{};
std::atomic_uintptr_t g_clientBegin{};
std::atomic_uintptr_t g_clientEnd{};
SRWLOCK g_installLock = SRWLOCK_INIT;
SRWLOCK g_registryLock = SRWLOCK_INIT;
SRWLOCK g_logLock = SRWLOCK_INIT;
std::unordered_map<HKEY, std::wstring> g_registryPaths;
std::unordered_set<std::wstring> g_loggedChanges;
std::once_flag g_profileOnce;
PrivacyProfile g_profile;

GetAdaptersInfoFn g_getAdaptersInfo{};
RegOpenKeyExWFn g_regOpenKeyExW{};
RegQueryValueExWFn g_regQueryValueExW{};
RegGetValueWFn g_regGetValueW{};
RegCloseKeyFn g_regCloseKey{};
WmiGetFn g_wmiGet{};

std::mt19937_64 Generator()
{
    std::random_device device;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::seed_seq seed{
        static_cast<unsigned int>(device()), static_cast<unsigned int>(device()),
        static_cast<unsigned int>(GetCurrentProcessId()),
        static_cast<unsigned int>(GetCurrentThreadId()),
        static_cast<unsigned int>(GetTickCount()),
        static_cast<unsigned int>(counter.LowPart),
        static_cast<unsigned int>(counter.HighPart)
    };
    return std::mt19937_64(seed);
}

std::wstring RandomText(std::mt19937_64& generator, std::wstring_view alphabet,
    std::size_t length)
{
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::wstring value;
    value.reserve(length);
    while(value.size() < length)
        value.push_back(alphabet[pick(generator)]);
    return value;
}

std::wstring Hex(std::mt19937_64& generator, std::size_t length)
{
    return RandomText(generator, L"0123456789ABCDEF", length);
}

std::wstring Digits(std::mt19937_64& generator, std::size_t length)
{
    return RandomText(generator, L"0123456789", length);
}

std::wstring AlphaNumeric(std::mt19937_64& generator, std::size_t length)
{
    return RandomText(generator, L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", length);
}

std::wstring Guid(std::mt19937_64& generator, bool braces)
{
    static constexpr wchar_t Variants[] = L"89AB";
    std::uniform_int_distribution<std::size_t> variant(0, std::size(Variants) - 2);
    std::wstring value = Hex(generator, 8) + L"-" + Hex(generator, 4) + L"-4"
        + Hex(generator, 3) + L"-" + Variants[variant(generator)]
        + Hex(generator, 3) + L"-" + Hex(generator, 12);
    return braces ? L"{" + value + L"}" : value;
}

unsigned int NumberAfter(std::wstring_view text, std::wstring_view key)
{
    const std::size_t position = text.find(key);
    if(position == std::wstring_view::npos)
        return 0;
    unsigned int value{};
    for(std::size_t index = position + key.size(); index < text.size(); ++index)
    {
        const wchar_t character = text[index];
        if(character < L'0' || character > L'9')
            break;
        value = value * 10 + static_cast<unsigned int>(character - L'0');
    }
    return value;
}

std::wstring ProcessorId(std::wstring_view identifier, bool amd)
{
    const unsigned int family = NumberAfter(identifier, L"Family ");
    const unsigned int model = NumberAfter(identifier, L"Model ");
    const unsigned int stepping = NumberAfter(identifier, L"Stepping ");
    const unsigned int baseFamily = (std::min)(family, 15u);
    const unsigned int signature = (stepping & 0xF) | ((model & 0xF) << 4)
        | ((baseFamily & 0xF) << 8) | (((model >> 4) & 0xF) << 16)
        | ((family > 15 ? family - 15 : 0) << 20);
    wchar_t value[17]{};
    swprintf_s(value, L"%08X%08X", amd ? 0x178BFBFFu : 0xBFEBFBFFu,
        signature);
    return value;
}

DWORD MicrocodeRevision(std::wstring_view identifier, bool amd)
{
    const unsigned int family = NumberAfter(identifier, L"Family ");
    const unsigned int model = NumberAfter(identifier, L"Model ");
    const unsigned int stepping = NumberAfter(identifier, L"Stepping ");
    if(!amd)
    {
        if(model == 60) return 0x00000028;
        if(model == 94) return 0x000000DC;
        if(model == 158 && stepping == 9) return 0x000000F2;
        if(model == 158) return 0x000000F4;
        if(model == 165) return 0x000000F8;
        if(model == 167) return 0x0000005D;
        if(model == 151) return 0x00000026;
        if(model == 191) return 0x00000032;
        if(model == 183) return 0x0000011D;
        if(model == 142) return 0x000000F6;
        if(model == 140) return 0x000000B8;
        return 0x00000020;
    }
    if(family == 23 && model == 1) return 0x08001138;
    if(family == 23 && model == 8) return 0x0800820D;
    if(family == 23 && model == 17) return 0x08101016;
    if(family == 23 && model == 24) return 0x08108102;
    if(family == 23 && model == 49) return 0x08301055;
    if(family == 23 && model == 96) return 0x0860010D;
    if(family == 23 && model == 113) return 0x08701030;
    if(family == 25 && model == 33) return 0x0A20120A;
    if(family == 25 && model == 80) return 0x0A50000F;
    return 0x08000020;
}

ULONGLONG FileTimeValue(const FILETIME& fileTime)
{
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

ULONGLONG FileTimeValue(unsigned int year, unsigned int month, unsigned int day)
{
    SYSTEMTIME systemTime{};
    systemTime.wYear = static_cast<WORD>(year);
    systemTime.wMonth = static_cast<WORD>(month);
    systemTime.wDay = static_cast<WORD>(day);
    FILETIME fileTime{};
    if(!SystemTimeToFileTime(&systemTime, &fileTime))
        return 0;
    return FileTimeValue(fileTime);
}

ULONGLONG CurrentFileTimeValue()
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    return FileTimeValue(fileTime);
}

bool Contains(std::wstring_view value, std::wstring_view part)
{
    return value.find(part) != std::wstring_view::npos;
}

std::wstring BoardCode(std::wstring_view product)
{
    const std::size_t position = product.find(L"MS-");
    if(position == std::wstring_view::npos || position + 7 > product.size())
        return L"0000";
    return std::wstring(product.substr(position + 3, 4));
}

std::wstring BoardSerial(std::mt19937_64& generator,
    std::wstring_view manufacturer, std::wstring_view product)
{
    if(Contains(manufacturer, L"ASUSTeK")) return AlphaNumeric(generator, 15);
    if(Contains(manufacturer, L"Gigabyte")) return L"SN" + Digits(generator, 12);
    if(Contains(manufacturer, L"Micro-Star"))
        return L"601-" + BoardCode(product) + L"-" + Digits(generator, 10);
    if(manufacturer == L"ASRock") return L"M80-" + Digits(generator, 10);
    if(Contains(manufacturer, L"Intel")) return L"BT" + AlphaNumeric(generator, 10);
    if(manufacturer == L"HP") return L"PG" + AlphaNumeric(generator, 8);
    if(manufacturer == L"LENOVO") return L"PF" + AlphaNumeric(generator, 8);
    if(Contains(manufacturer, L"Dell")) return AlphaNumeric(generator, 7);
    if(manufacturer == L"Acer") return L"NB" + AlphaNumeric(generator, 18);
    if(manufacturer == L"MEDION") return L"MED" + Digits(generator, 9);
    if(manufacturer == L"Biostar") return L"BIO" + AlphaNumeric(generator, 9);
    return AlphaNumeric(generator, 12);
}

std::wstring BiosSerial(std::mt19937_64& generator,
    std::wstring_view manufacturer)
{
    if(manufacturer == L"HP") return L"5CG" + AlphaNumeric(generator, 7);
    if(manufacturer == L"LENOVO") return L"PF" + AlphaNumeric(generator, 8);
    if(Contains(manufacturer, L"Dell")) return AlphaNumeric(generator, 7);
    if(manufacturer == L"Acer") return L"NX" + AlphaNumeric(generator, 10);
    if(manufacturer == L"MEDION") return AlphaNumeric(generator, 14);
    return AlphaNumeric(generator, 12);
}

std::wstring BiosVersion(std::mt19937_64& generator,
    std::wstring_view manufacturer, std::wstring_view product)
{
    if(Contains(manufacturer, L"ASUSTeK"))
    {
        std::uniform_int_distribution<unsigned int> version(1001, 4009);
        return std::to_wstring(version(generator));
    }
    if(Contains(manufacturer, L"Gigabyte"))
    {
        std::uniform_int_distribution<unsigned int> version(2, 32);
        return L"F" + std::to_wstring(version(generator));
    }
    if(Contains(manufacturer, L"Micro-Star"))
        return L"E" + BoardCode(product) + (Contains(product, L"MS-1")
            ? L"IMS." : L"AMS.") + Hex(generator, 3);
    if(manufacturer == L"ASRock")
        return L"P" + Digits(generator, 1) + L"." + Digits(generator, 2);
    if(Contains(manufacturer, L"Intel"))
        return std::wstring(product) + L".86A." + Digits(generator, 4);
    if(manufacturer == L"HP") return L"F." + Hex(generator, 2);
    if(manufacturer == L"LENOVO") return L"N3CN" + Digits(generator, 2) + L"W";
    if(Contains(manufacturer, L"Dell"))
        return Digits(generator, 1) + L"." + Digits(generator, 2) + L"."
            + Digits(generator, 1);
    if(manufacturer == L"Acer") return L"V1." + Digits(generator, 2);
    return Digits(generator, 1) + L"." + Digits(generator, 2);
}

template<std::size_t Size>
void RandomBytes(std::mt19937_64& generator, std::array<BYTE, Size>& bytes)
{
    std::uniform_int_distribution<unsigned int> pick(0, 255);
    for(BYTE& value : bytes)
        value = static_cast<BYTE>(pick(generator));
}

std::wstring BytesText(const void* data, std::size_t size)
{
    if(!data || !size)
        return L"<empty>";
    static constexpr wchar_t Digits[] = L"0123456789ABCDEF";
    const auto* bytes = static_cast<const BYTE*>(data);
    std::wstring text;
    text.reserve(size * 3 - 1);
    for(std::size_t index{}; index < size; ++index)
    {
        if(index)
            text.push_back(L'-');
        text.push_back(Digits[bytes[index] >> 4]);
        text.push_back(Digits[bytes[index] & 0x0F]);
    }
    return text;
}

void LogChange(std::wstring key, std::wstring_view value)
{
    AcquireSRWLockExclusive(&g_logLock);
    const bool first = g_loggedChanges.emplace(key).second;
    ReleaseSRWLockExclusive(&g_logLock);
    if(first)
        Log::Write(L"[privacy] substituted " + std::move(key) + L" => "
            + std::wstring(value));
}

void InitializePrivacyProfile()
{
    auto generator = Generator();
    g_profile.hwProfileGuid = Guid(generator, true);
    RandomBytes(generator, g_profile.clientHwid);
    g_profile.machineId = Guid(generator, true);
    static constexpr std::wstring_view ProductFamilies[]{
        L"00325", L"00326", L"00330", L"00331", L"00426", L"00427"
    };
    std::uniform_int_distribution<std::size_t> productFamily(0,
        std::size(ProductFamilies) - 1);
    g_profile.productId = std::wstring(ProductFamilies[productFamily(generator)])
        + L"-" + Digits(generator, 5) + L"-" + Digits(generator, 5) + L"-AAOEM";
    std::copy_n(reinterpret_cast<const BYTE*>("DMIO:ID:"), 8,
        g_profile.mountedDevice.begin());
    std::array<BYTE, 16> partitionGuid{};
    RandomBytes(generator, partitionGuid);
    partitionGuid[7] = static_cast<BYTE>((partitionGuid[7] & 0x0F) | 0x40);
    partitionGuid[8] = static_cast<BYTE>((partitionGuid[8] & 0x3F) | 0x80);
    std::copy(partitionGuid.begin(), partitionGuid.end(),
        g_profile.mountedDevice.begin() + 8);
    g_profile.machineGuid = Guid(generator, false);
    g_profile.systemUuid = Guid(generator, false);

    static constexpr std::wstring_view IntelProcessors[]{
        L"Intel(R) Core(TM) i3-4130 CPU @ 3.40GHz",
        L"Intel(R) Core(TM) i5-4460 CPU @ 3.20GHz",
        L"Intel(R) Core(TM) i7-4790 CPU @ 3.60GHz",
        L"Intel(R) Core(TM) i5-6500 CPU @ 3.20GHz",
        L"Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz",
        L"Intel(R) Core(TM) i5-7400 CPU @ 3.00GHz",
        L"Intel(R) Core(TM) i7-7700 CPU @ 3.60GHz",
        L"Intel(R) Core(TM) i5-8400 CPU @ 2.80GHz",
        L"Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz",
        L"Intel(R) Core(TM) i5-9400F CPU @ 2.90GHz",
        L"Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz",
        L"Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz",
        L"Intel(R) Core(TM) i5-10400F CPU @ 2.90GHz",
        L"Intel(R) Core(TM) i7-10700 CPU @ 2.90GHz",
        L"Intel(R) Core(TM) i7-10700K CPU @ 3.80GHz",
        L"11th Gen Intel(R) Core(TM) i5-11400F @ 2.60GHz",
        L"11th Gen Intel(R) Core(TM) i7-11700K @ 3.60GHz",
        L"12th Gen Intel(R) Core(TM) i5-12400F @ 2.50GHz",
        L"12th Gen Intel(R) Core(TM) i7-12700K @ 3.60GHz",
        L"13th Gen Intel(R) Core(TM) i5-13400F",
        L"13th Gen Intel(R) Core(TM) i7-13700K",
        L"Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz",
        L"Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz",
        L"Intel(R) Core(TM) i5-10300H CPU @ 2.50GHz",
        L"11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz"
    };
    static constexpr std::wstring_view AmdProcessors[]{
        L"AMD Ryzen 3 1200 Quad-Core Processor",
        L"AMD Ryzen 3 1300X Quad-Core Processor",
        L"AMD Ryzen 5 1600 Six-Core Processor",
        L"AMD Ryzen 7 1700 Eight-Core Processor",
        L"AMD Ryzen 3 2200G with Radeon Vega Graphics",
        L"AMD Ryzen 5 2400G with Radeon Vega Graphics",
        L"AMD Ryzen 5 2600 Six-Core Processor",
        L"AMD Ryzen 7 2700X Eight-Core Processor",
        L"AMD Ryzen 3 3100 4-Core Processor",
        L"AMD Ryzen 5 3600 6-Core Processor",
        L"AMD Ryzen 5 3600X 6-Core Processor",
        L"AMD Ryzen 7 3700X 8-Core Processor",
        L"AMD Ryzen 9 3900X 12-Core Processor",
        L"AMD Ryzen 9 3950X 16-Core Processor",
        L"AMD Ryzen 3 3300U with Radeon Vega Mobile Gfx",
        L"AMD Ryzen 5 3500U with Radeon Vega Mobile Gfx",
        L"AMD Ryzen 5 4500U with Radeon Graphics",
        L"AMD Ryzen 7 4800H with Radeon Graphics",
        L"AMD Ryzen 5 5500 6-Core Processor",
        L"AMD Ryzen 5 5600X 6-Core Processor",
        L"AMD Ryzen 7 5700G with Radeon Graphics",
        L"AMD Ryzen 7 5800X 8-Core Processor",
        L"AMD Ryzen 9 5900X 12-Core Processor",
        L"AMD Ryzen Threadripper 2950X 16-Core Processor",
        L"AMD Ryzen Threadripper 3960X 24-Core Processor"
    };
    static constexpr std::wstring_view IntelIdentifiers[]{
        L"Intel64 Family 6 Model 60 Stepping 3",
        L"Intel64 Family 6 Model 60 Stepping 3",
        L"Intel64 Family 6 Model 60 Stepping 3",
        L"Intel64 Family 6 Model 94 Stepping 3",
        L"Intel64 Family 6 Model 94 Stepping 3",
        L"Intel64 Family 6 Model 158 Stepping 9",
        L"Intel64 Family 6 Model 158 Stepping 9",
        L"Intel64 Family 6 Model 158 Stepping 10",
        L"Intel64 Family 6 Model 158 Stepping 10",
        L"Intel64 Family 6 Model 158 Stepping 13",
        L"Intel64 Family 6 Model 158 Stepping 13",
        L"Intel64 Family 6 Model 165 Stepping 3",
        L"Intel64 Family 6 Model 165 Stepping 3",
        L"Intel64 Family 6 Model 165 Stepping 5",
        L"Intel64 Family 6 Model 165 Stepping 5",
        L"Intel64 Family 6 Model 167 Stepping 1",
        L"Intel64 Family 6 Model 167 Stepping 1",
        L"Intel64 Family 6 Model 151 Stepping 5",
        L"Intel64 Family 6 Model 151 Stepping 2",
        L"Intel64 Family 6 Model 191 Stepping 2",
        L"Intel64 Family 6 Model 183 Stepping 1",
        L"Intel64 Family 6 Model 142 Stepping 10",
        L"Intel64 Family 6 Model 158 Stepping 13",
        L"Intel64 Family 6 Model 165 Stepping 2",
        L"Intel64 Family 6 Model 140 Stepping 1"
    };
    static constexpr std::wstring_view AmdIdentifiers[]{
        L"AMD64 Family 23 Model 1 Stepping 1",
        L"AMD64 Family 23 Model 1 Stepping 1",
        L"AMD64 Family 23 Model 1 Stepping 1",
        L"AMD64 Family 23 Model 1 Stepping 1",
        L"AMD64 Family 23 Model 17 Stepping 0",
        L"AMD64 Family 23 Model 17 Stepping 0",
        L"AMD64 Family 23 Model 8 Stepping 2",
        L"AMD64 Family 23 Model 8 Stepping 2",
        L"AMD64 Family 23 Model 113 Stepping 0",
        L"AMD64 Family 23 Model 113 Stepping 0",
        L"AMD64 Family 23 Model 113 Stepping 0",
        L"AMD64 Family 23 Model 113 Stepping 0",
        L"AMD64 Family 23 Model 113 Stepping 0",
        L"AMD64 Family 23 Model 113 Stepping 0",
        L"AMD64 Family 23 Model 24 Stepping 1",
        L"AMD64 Family 23 Model 24 Stepping 1",
        L"AMD64 Family 23 Model 96 Stepping 1",
        L"AMD64 Family 23 Model 96 Stepping 1",
        L"AMD64 Family 25 Model 80 Stepping 0",
        L"AMD64 Family 25 Model 33 Stepping 0",
        L"AMD64 Family 25 Model 80 Stepping 0",
        L"AMD64 Family 25 Model 33 Stepping 0",
        L"AMD64 Family 25 Model 33 Stepping 0",
        L"AMD64 Family 23 Model 8 Stepping 2",
        L"AMD64 Family 23 Model 49 Stepping 0"
    };
    static constexpr BoardModel IntelLga1150Boards[]{
        {L"Intel Corporation", L"DH87RL", L"Intel Corp."},
        {L"ASUSTeK COMPUTER INC.", L"H97-PLUS", L"American Megatrends Inc."}
    };
    static constexpr BoardModel IntelLga1151LegacyBoards[]{
        {L"ASUSTeK COMPUTER INC.", L"H110M-K", L"American Megatrends Inc."},
        {L"Gigabyte Technology Co., Ltd.", L"B250M-DS3H", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"B250M PRO-VDH (MS-7A70)", L"American Megatrends Inc."},
        {L"ASRock", L"H110M-HDV", L"American Megatrends Inc."}
    };
    static constexpr BoardModel IntelLga1151CoffeeBoards[]{
        {L"Gigabyte Technology Co., Ltd.", L"B360M DS3H", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"Z390-A PRO (MS-7B98)", L"American Megatrends Inc."},
        {L"ASRock", L"B365M Pro4", L"American Megatrends Inc."}
    };
    static constexpr BoardModel IntelLga1200Boards[]{
        {L"ASUSTeK COMPUTER INC.", L"PRIME Z490-P", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"Z490-A PRO (MS-7C75)", L"American Megatrends Inc."},
        {L"Gigabyte Technology Co., Ltd.", L"B560M DS3H", L"American Megatrends International, LLC."},
        {L"ASRock", L"B560M Pro4", L"American Megatrends International, LLC."}
    };
    static constexpr BoardModel IntelLga1700Boards[]{
        {L"ASUSTeK COMPUTER INC.", L"PRIME B660-PLUS D4", L"American Megatrends International, LLC."},
        {L"Gigabyte Technology Co., Ltd.", L"B660M DS3H DDR4", L"American Megatrends International, LLC."},
        {L"Micro-Star International Co., Ltd.", L"PRO Z690-A DDR4 (MS-7D25)", L"American Megatrends International, LLC."}
    };
    static constexpr BoardModel IntelMobileBoards[]{
        {L"HP", L"84A6", L"Insyde"},
        {L"LENOVO", L"LNVNB161216", L"LENOVO"},
        {L"Dell Inc.", L"0M6C7G", L"Dell Inc."},
        {L"Acer", L"KBL Charmander_KL", L"Insyde Corp."},
        {L"MEDION", L"N15_17RD", L"American Megatrends Inc."},
        {L"ASUSTeK COMPUTER INC.", L"X510UAR", L"American Megatrends Inc."}
    };
    static constexpr BoardModel AmdLegacyAm4Boards[]{
        {L"ASUSTeK COMPUTER INC.", L"PRIME B450-PLUS", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"B450 TOMAHAWK MAX (MS-7C02)", L"American Megatrends Inc."},
        {L"Gigabyte Technology Co., Ltd.", L"B450 AORUS ELITE", L"American Megatrends Inc."},
        {L"ASRock", L"B450M Pro4", L"American Megatrends Inc."},
        {L"Biostar", L"B450MH", L"American Megatrends Inc."}
    };
    static constexpr BoardModel AmdModernAm4Boards[]{
        {L"ASUSTeK COMPUTER INC.", L"PRIME B450-PLUS", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"B450 TOMAHAWK MAX (MS-7C02)", L"American Megatrends Inc."},
        {L"ASUSTeK COMPUTER INC.", L"TUF GAMING B550-PLUS", L"American Megatrends International, LLC."},
        {L"Gigabyte Technology Co., Ltd.", L"B550M DS3H", L"American Megatrends International, LLC."},
        {L"Micro-Star International Co., Ltd.", L"B550-A PRO (MS-7C56)", L"American Megatrends International, LLC."},
        {L"ASRock", L"B550M Pro4", L"American Megatrends International, LLC."}
    };
    static constexpr BoardModel AmdMobileBoards[]{
        {L"LENOVO", L"LNVNB161216", L"LENOVO"},
        {L"HP", L"87C3", L"AMI"},
        {L"Acer", L"Octavia_PKS", L"Insyde Corp."},
        {L"ASUSTeK COMPUTER INC.", L"FA506IU", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"MS-17FK", L"American Megatrends International, LLC."}
    };
    static constexpr BoardModel ThreadripperX399Boards[]{
        {L"ASUSTeK COMPUTER INC.", L"ROG ZENITH EXTREME", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"MEG X399 CREATION (MS-7B92)", L"American Megatrends Inc."},
        {L"Gigabyte Technology Co., Ltd.", L"X399 AORUS PRO", L"American Megatrends Inc."}
    };
    static constexpr BoardModel ThreadripperTrx40Boards[]{
        {L"ASUSTeK COMPUTER INC.", L"ROG ZENITH II EXTREME", L"American Megatrends Inc."},
        {L"Micro-Star International Co., Ltd.", L"Creator TRX40 (MS-7C59)", L"American Megatrends Inc."},
        {L"Gigabyte Technology Co., Ltd.", L"TRX40 AORUS MASTER", L"American Megatrends Inc."}
    };
    static constexpr std::wstring_view LegacyAdapters[]{
        L"AMD Radeon R9 270X", L"AMD Radeon R9 380 Series",
        L"AMD Radeon RX 460", L"AMD Radeon RX 470", L"AMD Radeon RX 480",
        L"AMD Radeon RX 570 Series", L"AMD Radeon RX 580 Series",
        L"NVIDIA GeForce GT 730", L"NVIDIA GeForce GTX 750 Ti",
        L"NVIDIA GeForce GTX 950", L"NVIDIA GeForce GTX 960",
        L"NVIDIA GeForce GTX 970", L"NVIDIA GeForce GTX 980",
        L"NVIDIA GeForce GTX 1050", L"NVIDIA GeForce GTX 1050 Ti",
        L"NVIDIA GeForce GTX 1060 3GB", L"NVIDIA GeForce GTX 1060 6GB",
        L"NVIDIA GeForce GTX 1070", L"NVIDIA GeForce GTX 1080"
    };
    static constexpr std::wstring_view ModernAdapters[]{
        L"AMD Radeon RX 570 Series", L"AMD Radeon RX 580 Series",
        L"AMD Radeon RX 5500 XT", L"AMD Radeon RX 5600 XT",
        L"AMD Radeon RX 5700 XT", L"AMD Radeon RX 6600",
        L"AMD Radeon RX 6700 XT", L"NVIDIA GeForce GTX 1050 Ti",
        L"NVIDIA GeForce GTX 1060 6GB", L"NVIDIA GeForce GTX 1070",
        L"NVIDIA GeForce GTX 1080", L"NVIDIA GeForce GTX 1650",
        L"NVIDIA GeForce GTX 1660", L"NVIDIA GeForce GTX 1660 SUPER",
        L"NVIDIA GeForce RTX 2060", L"NVIDIA GeForce RTX 2070",
        L"NVIDIA GeForce RTX 2080", L"NVIDIA GeForce RTX 3050",
        L"NVIDIA GeForce RTX 3060", L"NVIDIA GeForce RTX 3070",
        L"NVIDIA GeForce RTX 3080"
    };
    static constexpr std::wstring_view MobileAdapters[]{
        L"Intel(R) HD Graphics 620", L"Intel(R) UHD Graphics 620",
        L"Intel(R) UHD Graphics 630", L"Intel(R) Iris(R) Xe Graphics",
        L"NVIDIA GeForce MX150", L"NVIDIA GeForce 940MX",
        L"NVIDIA GeForce GTX 1050", L"NVIDIA GeForce GTX 1650",
        L"NVIDIA GeForce GTX 1660 Ti", L"NVIDIA GeForce RTX 2060"
    };
    static_assert(std::size(IntelProcessors) == std::size(IntelIdentifiers));
    static_assert(std::size(AmdProcessors) == std::size(AmdIdentifiers));
    std::uniform_int_distribution<unsigned int> vendor(0, 1);
    const bool amd = vendor(generator) != 0;
    HardwarePlatform platform{};
    std::size_t processorIndex{};
    if(amd)
    {
        std::uniform_int_distribution<std::size_t> processor(0,
            std::size(AmdProcessors) - 1);
        const std::size_t index = processor(generator);
        processorIndex = index;
        g_profile.processorName = AmdProcessors[index];
        g_profile.processorIdentifier = AmdIdentifiers[index];
        if(index == 23)
            platform = HardwarePlatform::ThreadripperX399;
        else if(index == 24)
            platform = HardwarePlatform::ThreadripperTrx40;
        else if(index >= 14 && index <= 17)
            platform = HardwarePlatform::AmdMobile;
        else if(index <= 7)
            platform = HardwarePlatform::AmdLegacyAm4;
        else
            platform = HardwarePlatform::AmdModernAm4;
    }
    else
    {
        std::uniform_int_distribution<std::size_t> processor(0,
            std::size(IntelProcessors) - 1);
        const std::size_t index = processor(generator);
        processorIndex = index;
        g_profile.processorName = IntelProcessors[index];
        g_profile.processorIdentifier = IntelIdentifiers[index];
        if(index >= 21)
            platform = HardwarePlatform::IntelMobile;
        else if(index <= 2)
            platform = HardwarePlatform::IntelLga1150;
        else if(index <= 6)
            platform = HardwarePlatform::IntelLga1151Legacy;
        else if(index <= 10)
            platform = HardwarePlatform::IntelLga1151Coffee;
        else if(index <= 16)
            platform = HardwarePlatform::IntelLga1200;
        else
            platform = HardwarePlatform::IntelLga1700;
    }
    g_profile.processorVendor = amd ? L"AuthenticAMD" : L"GenuineIntel";
    g_profile.processorId = ProcessorId(g_profile.processorIdentifier, amd);
    const DWORD revision = MicrocodeRevision(g_profile.processorIdentifier, amd);
    std::memcpy(g_profile.processorRevision.data(), &revision, sizeof(revision));
    const auto selectBoard = [&](const auto& boards)
    {
        std::uniform_int_distribution<std::size_t> select(0,
            std::size(boards) - 1);
        const BoardModel& model = boards[select(generator)];
        g_profile.baseBoardManufacturer = model.manufacturer;
        g_profile.baseBoardProduct = model.product;
        g_profile.biosVendor = model.biosVendor;
    };
    switch(platform)
    {
        case HardwarePlatform::IntelLga1150: selectBoard(IntelLga1150Boards); break;
        case HardwarePlatform::IntelLga1151Legacy: selectBoard(IntelLga1151LegacyBoards); break;
        case HardwarePlatform::IntelLga1151Coffee: selectBoard(IntelLga1151CoffeeBoards); break;
        case HardwarePlatform::IntelLga1200: selectBoard(IntelLga1200Boards); break;
        case HardwarePlatform::IntelLga1700: selectBoard(IntelLga1700Boards); break;
        case HardwarePlatform::IntelMobile: selectBoard(IntelMobileBoards); break;
        case HardwarePlatform::AmdLegacyAm4: selectBoard(AmdLegacyAm4Boards); break;
        case HardwarePlatform::AmdModernAm4: selectBoard(AmdModernAm4Boards); break;
        case HardwarePlatform::AmdMobile: selectBoard(AmdMobileBoards); break;
        case HardwarePlatform::ThreadripperX399: selectBoard(ThreadripperX399Boards); break;
        case HardwarePlatform::ThreadripperTrx40: selectBoard(ThreadripperTrx40Boards); break;
    }
    unsigned int minimumBiosYear{};
    unsigned int maximumBiosYear{};
    switch(platform)
    {
        case HardwarePlatform::IntelLga1150: minimumBiosYear = 2013; maximumBiosYear = 2018; break;
        case HardwarePlatform::IntelLga1151Legacy: minimumBiosYear = 2015; maximumBiosYear = 2020; break;
        case HardwarePlatform::IntelLga1151Coffee: minimumBiosYear = 2017; maximumBiosYear = 2022; break;
        case HardwarePlatform::IntelLga1200: minimumBiosYear = 2020; maximumBiosYear = 2023; break;
        case HardwarePlatform::IntelLga1700: minimumBiosYear = 2022; maximumBiosYear = 2025; break;
        case HardwarePlatform::IntelMobile: minimumBiosYear = 2018; maximumBiosYear = 2023; break;
        case HardwarePlatform::AmdLegacyAm4: minimumBiosYear = 2017; maximumBiosYear = 2022; break;
        case HardwarePlatform::AmdModernAm4: minimumBiosYear = 2019; maximumBiosYear = 2025; break;
        case HardwarePlatform::AmdMobile: minimumBiosYear = 2018; maximumBiosYear = 2023; break;
        case HardwarePlatform::ThreadripperX399: minimumBiosYear = 2017; maximumBiosYear = 2021; break;
        case HardwarePlatform::ThreadripperTrx40: minimumBiosYear = 2019; maximumBiosYear = 2023; break;
    }
    g_profile.baseBoardSerial = BoardSerial(generator,
        g_profile.baseBoardManufacturer, g_profile.baseBoardProduct);
    g_profile.biosSerial = BiosSerial(generator, g_profile.baseBoardManufacturer);
    g_profile.biosVersion = BiosVersion(generator,
        g_profile.baseBoardManufacturer, g_profile.baseBoardProduct);
    std::uniform_int_distribution<unsigned int> month(1, 12);
    std::uniform_int_distribution<unsigned int> day(1, 28);
    std::uniform_int_distribution<unsigned int> year(minimumBiosYear,
        maximumBiosYear);
    const unsigned int biosMonth = month(generator);
    const unsigned int biosDay = day(generator);
    const unsigned int biosYear = year(generator);
    wchar_t releaseDate[16]{};
    swprintf_s(releaseDate, L"%02u/%02u/%04u", biosMonth, biosDay, biosYear);
    g_profile.biosReleaseDate = releaseDate;
    wchar_t wmiReleaseDate[32]{};
    swprintf_s(wmiReleaseDate, L"%04u%02u%02u000000.000000+000",
        biosYear, biosMonth, biosDay);
    g_profile.biosWmiReleaseDate = wmiReleaseDate;
    std::uniform_int_distribution<std::uint32_t> sidPart(1, UINT32_MAX);
    std::uniform_int_distribution<unsigned int> rid(1000, 9999);
    g_profile.autoLogonSid = L"S-1-5-21-" + std::to_wstring(sidPart(generator))
        + L"-" + std::to_wstring(sidPart(generator)) + L"-"
        + std::to_wstring(sidPart(generator)) + L"-" + std::to_wstring(rid(generator));
    const ULONGLONG now = CurrentFileTimeValue();
    const ULONGLONG latestEdgeTime = now - EdgeMinimumAge;
    const ULONGLONG edgeEpoch = FileTimeValue(2020, 1, 15);
    const ULONGLONG earliestEdgeTime = (std::max)(now - EdgeMaximumAge,
        edgeEpoch);
    std::uniform_int_distribution<ULONGLONG> edgeTime(earliestEdgeTime,
        latestEdgeTime);
    g_profile.edgeInstallTime = edgeTime(generator);
    const auto selectAdapter = [&](const auto& adapters)
    {
        std::uniform_int_distribution<std::size_t> select(0,
            std::size(adapters) - 1);
        g_profile.primaryAdapter = adapters[select(generator)];
    };
    if(amd && processorIndex == 4) g_profile.primaryAdapter = L"AMD Radeon(TM) Vega 8 Graphics";
    else if(amd && processorIndex == 5) g_profile.primaryAdapter = L"AMD Radeon(TM) RX Vega 11 Graphics";
    else if(amd && processorIndex == 14) g_profile.primaryAdapter = L"AMD Radeon(TM) Vega 6 Graphics";
    else if(amd && (processorIndex == 15 || processorIndex == 20))
        g_profile.primaryAdapter = L"AMD Radeon(TM) Vega 8 Graphics";
    else if(amd && (processorIndex == 16 || processorIndex == 17))
        g_profile.primaryAdapter = L"AMD Radeon(TM) Graphics";
    else if(!amd && processorIndex == 21) g_profile.primaryAdapter = L"Intel(R) UHD Graphics 620";
    else if(!amd && (processorIndex == 22 || processorIndex == 23))
        g_profile.primaryAdapter = L"Intel(R) UHD Graphics 630";
    else if(!amd && processorIndex == 24) g_profile.primaryAdapter = L"Intel(R) Iris(R) Xe Graphics";
    else if(platform == HardwarePlatform::IntelLga1150
        || platform == HardwarePlatform::IntelLga1151Legacy
        || platform == HardwarePlatform::AmdLegacyAm4)
        selectAdapter(LegacyAdapters);
    else if(platform == HardwarePlatform::IntelMobile
        || platform == HardwarePlatform::AmdMobile)
        selectAdapter(MobileAdapters);
    else
        selectAdapter(ModernAdapters);
    static constexpr std::array<std::array<BYTE, 3>, 5> Ouis{{
        {0x00, 0xE0, 0x4C}, {0x00, 0x1B, 0x21}, {0x3C, 0x97, 0x0E},
        {0xA4, 0xBB, 0x6D}, {0xF8, 0x63, 0x3F}
    }};
    std::uniform_int_distribution<std::size_t> oui(0, Ouis.size() - 1);
    const auto& selectedOui = Ouis[oui(generator)];
    std::copy(selectedOui.begin(), selectedOui.end(), g_profile.mac.begin());
    std::array<BYTE, 3> nic{};
    RandomBytes(generator, nic);
    std::copy(nic.begin(), nic.end(), g_profile.mac.begin() + 3);
}

bool Hexadecimal(wchar_t character)
{
    return (character >= L'0' && character <= L'9')
        || (character >= L'A' && character <= L'F');
}

bool ValidGuid(std::wstring_view value, bool braces)
{
    if(value.size() != (braces ? 38u : 36u))
        return false;
    if(braces)
    {
        if(value.front() != L'{' || value.back() != L'}')
            return false;
        value = value.substr(1, 36);
    }
    for(std::size_t index{}; index < value.size(); ++index)
    {
        const bool separator = index == 8 || index == 13 || index == 18
            || index == 23;
        if(separator ? value[index] != L'-' : !Hexadecimal(value[index]))
            return false;
    }
    return value[14] == L'4' && Contains(L"89AB", value.substr(19, 1));
}

bool ValidPrivacyProfile()
{
    const bool clientHwid = std::any_of(g_profile.clientHwid.begin(),
        g_profile.clientHwid.end(), [](BYTE value) { return value != 0; });
    const bool revision = std::any_of(g_profile.processorRevision.begin(),
        g_profile.processorRevision.end(), [](BYTE value) { return value != 0; });
    const bool productId = g_profile.productId.size() == 23
        && g_profile.productId[5] == L'-' && g_profile.productId[11] == L'-'
        && g_profile.productId[17] == L'-'
        && g_profile.productId.substr(18) == L"AAOEM";
    const bool mounted = !std::memcmp(g_profile.mountedDevice.data(),
        "DMIO:ID:", 8);
    const bool processorId = g_profile.processorId.size() == 16
        && std::all_of(g_profile.processorId.begin(), g_profile.processorId.end(),
            &Hexadecimal);
    const bool biosDate = g_profile.biosReleaseDate.size() == 10
        && g_profile.biosReleaseDate[2] == L'/'
        && g_profile.biosReleaseDate[5] == L'/';
    const bool wmiDate = g_profile.biosWmiReleaseDate.size() == 25
        && g_profile.biosWmiReleaseDate[14] == L'.'
        && g_profile.biosWmiReleaseDate[21] == L'+';
    const ULONGLONG now = CurrentFileTimeValue();
    const bool edgeTime = g_profile.edgeInstallTime >= FileTimeValue(2020, 1, 15)
        && g_profile.edgeInstallTime <= now - EdgeMinimumAge;
    return ValidGuid(g_profile.hwProfileGuid, true)
        && ValidGuid(g_profile.machineId, true)
        && ValidGuid(g_profile.machineGuid, false)
        && ValidGuid(g_profile.systemUuid, false) && clientHwid && productId
        && mounted && revision && processorId && biosDate && wmiDate
        && !g_profile.processorName.empty() && !g_profile.processorVendor.empty()
        && !g_profile.processorIdentifier.empty()
        && !g_profile.baseBoardManufacturer.empty()
        && !g_profile.baseBoardProduct.empty() && !g_profile.baseBoardSerial.empty()
        && !g_profile.biosSerial.empty() && !g_profile.biosVendor.empty()
        && !g_profile.biosVersion.empty()
        && g_profile.autoLogonSid.starts_with(L"S-1-5-21-")
        && edgeTime && !g_profile.primaryAdapter.empty()
        && !(g_profile.mac[0] & 0x03);
}

bool CalledFromClient(void* returnAddress)
{
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(returnAddress);
    const std::uintptr_t begin = g_clientBegin.load(std::memory_order_acquire);
    const std::uintptr_t end = g_clientEnd.load(std::memory_order_acquire);
    return begin && address >= begin && address < end;
}

std::wstring Normalize(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
    {
        return character == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(character));
    });
    constexpr std::wstring_view Wow = L"hklm\\software\\wow6432node\\";
    if(value.starts_with(Wow))
        value.replace(0, Wow.size(), L"hklm\\software\\");
    while(!value.empty() && value.back() == L'\\')
        value.pop_back();
    return value;
}

std::wstring RootPath(HKEY key)
{
    if(key == HKEY_LOCAL_MACHINE) return L"hklm";
    if(key == HKEY_CURRENT_USER) return L"hkcu";
    if(key == HKEY_CLASSES_ROOT) return L"hkcr";
    if(key == HKEY_USERS) return L"hku";
    if(key == HKEY_CURRENT_CONFIG) return L"hkcc";
    AcquireSRWLockShared(&g_registryLock);
    const auto found = g_registryPaths.find(key);
    std::wstring path = found == g_registryPaths.end() ? std::wstring{} : found->second;
    ReleaseSRWLockShared(&g_registryLock);
    return path;
}

std::wstring JoinPath(HKEY key, LPCWSTR subKey)
{
    std::wstring path = RootPath(key);
    if(path.empty())
        return {};
    if(subKey && *subKey)
    {
        path.push_back(L'\\');
        path.append(subKey);
    }
    return Normalize(std::move(path));
}

bool Same(LPCWSTR left, std::wstring_view right)
{
    return left && !_wcsicmp(left, std::wstring(right).c_str());
}

FakeValue StringValue(const std::wstring& value)
{
    return {REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1)
        * sizeof(wchar_t))};
}

template<std::size_t Size>
FakeValue BinaryValue(const std::array<BYTE, Size>& value)
{
    return {REG_BINARY, value.data(), static_cast<DWORD>(value.size())};
}

FakeValue QwordValue(const ULONGLONG& value)
{
    return {REG_QWORD, &value, sizeof(value)};
}

std::wstring FakeValueText(const FakeValue& value)
{
    if(value.type == REG_SZ && value.data && value.size >= sizeof(wchar_t))
        return static_cast<const wchar_t*>(value.data);
    if(value.type == REG_QWORD && value.data && value.size == sizeof(ULONGLONG))
    {
        ULONGLONG number{};
        std::memcpy(&number, value.data, sizeof(number));
        wchar_t text[19]{};
        swprintf_s(text, L"0x%016llX", number);
        return text;
    }
    return BytesText(value.data, value.size);
}

bool FakeRegistryValue(std::wstring_view path, LPCWSTR name, FakeValue& output)
{
    if(PrivacyFlags::Registry::HardwareProfileGuid
        && path.ends_with(L"system\\currentcontrolset\\control\\idconfigdb\\hardware profiles\\0001")
        && Same(name, L"HwProfileGuid"))
        output = StringValue(g_profile.hwProfileGuid);
    else if(PrivacyFlags::Registry::ClientHwid
        && path.ends_with(L"software\\microsoft\\mslicensing\\hardwareid")
        && Same(name, L"ClientHWID"))
        output = BinaryValue(g_profile.clientHwid);
    else if(PrivacyFlags::Registry::SqmMachineId
        && path.ends_with(L"software\\microsoft\\sqmclient")
        && Same(name, L"MachineId"))
        output = StringValue(g_profile.machineId);
    else if(PrivacyFlags::Registry::WindowsProductId
        && path.ends_with(L"software\\microsoft\\windows nt\\currentversion")
        && Same(name, L"ProductId"))
        output = StringValue(g_profile.productId);
    else if(PrivacyFlags::Registry::MountedSystemDrive
        && path.ends_with(L"system\\mounteddevices")
        && Same(name, L"\\DosDevices\\C:"))
        output = BinaryValue(g_profile.mountedDevice);
    else if(PrivacyFlags::Registry::MachineGuid
        && path.ends_with(L"software\\microsoft\\cryptography")
        && Same(name, L"MachineGuid"))
        output = StringValue(g_profile.machineGuid);
    else if(PrivacyFlags::Registry::ProcessorName
        && path.ends_with(L"hardware\\description\\system\\centralprocessor\\0")
        && Same(name, L"ProcessorNameString"))
        output = StringValue(g_profile.processorName);
    else if(PrivacyFlags::Registry::ProcessorVendor
        && path.ends_with(L"hardware\\description\\system\\centralprocessor\\0")
        && Same(name, L"VendorIdentifier"))
        output = StringValue(g_profile.processorVendor);
    else if(PrivacyFlags::Registry::ProcessorIdentifier
        && path.ends_with(L"hardware\\description\\system\\centralprocessor\\0")
        && Same(name, L"Identifier"))
        output = StringValue(g_profile.processorIdentifier);
    else if(PrivacyFlags::Registry::ProcessorRevision
        && path.ends_with(L"hardware\\description\\system\\centralprocessor\\0")
        && Same(name, L"Update Revision"))
        output = BinaryValue(g_profile.processorRevision);
    else if(PrivacyFlags::Registry::BaseBoardManufacturer
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BaseBoardManufacturer"))
        output = StringValue(g_profile.baseBoardManufacturer);
    else if(PrivacyFlags::Registry::BaseBoardProduct
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BaseBoardProduct"))
        output = StringValue(g_profile.baseBoardProduct);
    else if(PrivacyFlags::Registry::BaseBoardSerial
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BaseBoardSerialNumber"))
        output = StringValue(g_profile.baseBoardSerial);
    else if(PrivacyFlags::Registry::BiosSerial
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BIOSSerialNumber"))
        output = StringValue(g_profile.biosSerial);
    else if(PrivacyFlags::Registry::BiosReleaseDate
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BIOSReleaseDate"))
        output = StringValue(g_profile.biosReleaseDate);
    else if(PrivacyFlags::Registry::BiosVendor
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BIOSVendor"))
        output = StringValue(g_profile.biosVendor);
    else if(PrivacyFlags::Registry::BiosVersion
        && path.ends_with(L"hardware\\description\\system\\bios")
        && Same(name, L"BIOSVersion"))
        output = StringValue(g_profile.biosVersion);
    else if(PrivacyFlags::Registry::AutoLogonSid
        && path.ends_with(L"software\\microsoft\\windows nt\\currentversion\\winlogon")
        && (Same(name, L"AutoLogonSID") || Same(name, L"AutoLogonSSID")))
        output = StringValue(g_profile.autoLogonSid);
    else if(PrivacyFlags::Registry::EdgeInstallTime
        && path.ends_with(L"software\\microsoft\\edge\\ietoedge")
        && Same(name, L"AlwaysEdgeInstallTime"))
        output = QwordValue(g_profile.edgeInstallTime);
    else if(PrivacyFlags::Registry::PrimaryAdapter
        && path.ends_with(L"software\\microsoft\\windows nt\\currentversion\\winsat")
        && Same(name, L"PrimaryAdapterString"))
        output = StringValue(g_profile.primaryAdapter);
    else
        return false;
    return true;
}

LSTATUS CopyFakeValue(const FakeValue& value, LPDWORD type, void* data,
    LPDWORD size)
{
    if(!size)
        return ERROR_INVALID_PARAMETER;
    const DWORD capacity = *size;
    *size = value.size;
    if(type)
        *type = value.type;
    if(!data)
        return ERROR_SUCCESS;
    if(capacity < value.size)
        return ERROR_MORE_DATA;
    std::memcpy(data, value.data, value.size);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI HookRegOpenKeyExW(HKEY key, LPCWSTR subKey, DWORD options,
    REGSAM access, PHKEY result)
{
    const bool clientCall = CalledFromClient(_ReturnAddress());
    const LSTATUS status = g_regOpenKeyExW(key, subKey, options, access, result);
    if(PrivacyFlags::Registry::Enabled && clientCall
        && status == ERROR_SUCCESS && result && *result)
    {
        std::wstring path = JoinPath(key, subKey);
        if(!path.empty())
        {
            AcquireSRWLockExclusive(&g_registryLock);
            g_registryPaths[*result] = std::move(path);
            ReleaseSRWLockExclusive(&g_registryLock);
        }
    }
    return status;
}

LSTATUS WINAPI HookRegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved,
    LPDWORD type, LPBYTE data, LPDWORD size)
{
    if(PrivacyFlags::Registry::Enabled && CalledFromClient(_ReturnAddress()))
    {
        const std::wstring path = RootPath(key);
        FakeValue value;
        if(FakeRegistryValue(path, name, value))
        {
            const LSTATUS status = CopyFakeValue(value, type, data, size);
            if(status == ERROR_SUCCESS && data)
                LogChange(L"registry " + path + L"\\" + name,
                    FakeValueText(value));
            return status;
        }
    }
    return g_regQueryValueExW(key, name, reserved, type, data, size);
}

LSTATUS WINAPI HookRegGetValueW(HKEY key, LPCWSTR subKey, LPCWSTR name,
    DWORD flags, LPDWORD type, PVOID data, LPDWORD size)
{
    if(PrivacyFlags::Registry::Enabled && CalledFromClient(_ReturnAddress()))
    {
        const std::wstring path = JoinPath(key, subKey);
        FakeValue value;
        if(FakeRegistryValue(path, name, value))
        {
            const LSTATUS status = CopyFakeValue(value, type, data, size);
            if(status == ERROR_SUCCESS && data)
                LogChange(L"registry " + path + L"\\" + name,
                    FakeValueText(value));
            return status;
        }
    }
    return g_regGetValueW(key, subKey, name, flags, type, data, size);
}

LSTATUS WINAPI HookRegCloseKey(HKEY key)
{
    AcquireSRWLockExclusive(&g_registryLock);
    g_registryPaths.erase(key);
    ReleaseSRWLockExclusive(&g_registryLock);
    return g_regCloseKey(key);
}

ULONG WINAPI HookGetAdaptersInfo(PIP_ADAPTER_INFO adapters, PULONG size)
{
    const bool clientCall = CalledFromClient(_ReturnAddress());
    const ULONG status = g_getAdaptersInfo(adapters, size);
    if(!PrivacyFlags::Adapters::Enabled || !PrivacyFlags::Adapters::MacAddress
        || !clientCall || status != ERROR_SUCCESS)
        return status;
    unsigned int index{};
    for(PIP_ADAPTER_INFO adapter = adapters; adapter; adapter = adapter->Next, ++index)
    {
        if(adapter->AddressLength < g_profile.mac.size())
            continue;
        std::copy(g_profile.mac.begin(), g_profile.mac.end(), adapter->Address);
        adapter->Address[5] = static_cast<BYTE>(adapter->Address[5] + index);
        LogChange(L"GetAdaptersInfo MAC[" + std::to_wstring(index) + L"]",
            BytesText(adapter->Address, g_profile.mac.size()));
    }
    return status;
}

bool ReplaceVariant(VARIANT* value, const std::wstring& replacement)
{
    if(!value)
        return false;
    BSTR text = SysAllocStringLen(replacement.data(),
        static_cast<UINT>(replacement.size()));
    if(!text)
        return false;
    VariantClear(value);
    value->vt = VT_BSTR;
    value->bstrVal = text;
    return true;
}

HRESULT WINAPI HookWmiGet(void* self, LPCWSTR name, long flags, VARIANT* value,
    CIMTYPE* type, long* flavor)
{
    const bool clientCall = CalledFromClient(_ReturnAddress());
    const HRESULT status = g_wmiGet(self, name, flags, value, type, flavor);
    if(!PrivacyFlags::Wmi::Enabled || !clientCall
        || FAILED(status) || !name || !value)
        return status;

    VARIANT classValue;
    VariantInit(&classValue);
    const HRESULT classStatus = g_wmiGet(self, L"__CLASS", 0, &classValue,
        nullptr, nullptr);
    if(FAILED(classStatus) || classValue.vt != VT_BSTR || !classValue.bstrVal)
    {
        VariantClear(&classValue);
        return status;
    }
    const std::wstring_view className(classValue.bstrVal,
        SysStringLen(classValue.bstrVal));
    const std::wstring* replacement{};
    if(PrivacyFlags::Wmi::VideoCaption
        && className == L"Win32_VideoController" && Same(name, L"Caption"))
        replacement = &g_profile.primaryAdapter;
    else if(PrivacyFlags::Wmi::VideoName
        && className == L"Win32_VideoController" && Same(name, L"Name"))
        replacement = &g_profile.primaryAdapter;
    else if(PrivacyFlags::Wmi::VideoDescription
        && className == L"Win32_VideoController" && Same(name, L"Description"))
        replacement = &g_profile.primaryAdapter;
    else if(PrivacyFlags::Wmi::BaseBoardSerial
        && className == L"Win32_BaseBoard" && Same(name, L"SerialNumber"))
        replacement = &g_profile.baseBoardSerial;
    else if(PrivacyFlags::Wmi::BaseBoardProduct
        && className == L"Win32_BaseBoard" && Same(name, L"Product"))
        replacement = &g_profile.baseBoardProduct;
    else if(PrivacyFlags::Wmi::BaseBoardManufacturer
        && className == L"Win32_BaseBoard" && Same(name, L"Manufacturer"))
        replacement = &g_profile.baseBoardManufacturer;
    else if(PrivacyFlags::Wmi::BiosSerial
        && className == L"Win32_BIOS" && Same(name, L"SerialNumber"))
        replacement = &g_profile.biosSerial;
    else if(PrivacyFlags::Wmi::BiosManufacturer
        && className == L"Win32_BIOS" && Same(name, L"Manufacturer"))
        replacement = &g_profile.biosVendor;
    else if(PrivacyFlags::Wmi::BiosSmbiosVersion
        && className == L"Win32_BIOS" && Same(name, L"SMBIOSBIOSVersion"))
        replacement = &g_profile.biosVersion;
    else if(PrivacyFlags::Wmi::BiosVersion
        && className == L"Win32_BIOS" && Same(name, L"Version"))
        replacement = &g_profile.biosVersion;
    else if(PrivacyFlags::Wmi::BiosReleaseDate
        && className == L"Win32_BIOS" && Same(name, L"ReleaseDate"))
        replacement = &g_profile.biosWmiReleaseDate;
    else if(PrivacyFlags::Wmi::ProcessorName
        && className == L"Win32_Processor" && Same(name, L"Name"))
        replacement = &g_profile.processorName;
    else if(PrivacyFlags::Wmi::ProcessorManufacturer
        && className == L"Win32_Processor" && Same(name, L"Manufacturer"))
        replacement = &g_profile.processorVendor;
    else if(PrivacyFlags::Wmi::ProcessorDescription
        && className == L"Win32_Processor" && Same(name, L"Description"))
        replacement = &g_profile.processorIdentifier;
    else if(PrivacyFlags::Wmi::ProcessorId
        && className == L"Win32_Processor" && Same(name, L"ProcessorId"))
        replacement = &g_profile.processorId;
    else if(PrivacyFlags::Wmi::SystemProductUuid
        && className == L"Win32_ComputerSystemProduct" && Same(name, L"UUID"))
        replacement = &g_profile.systemUuid;
    else if(PrivacyFlags::Wmi::SystemProductIdentifyingNumber
        && className == L"Win32_ComputerSystemProduct" && Same(name, L"IdentifyingNumber"))
        replacement = &g_profile.biosSerial;
    else if(PrivacyFlags::Wmi::SystemProductVendor
        && className == L"Win32_ComputerSystemProduct" && Same(name, L"Vendor"))
        replacement = &g_profile.baseBoardManufacturer;
    else if(PrivacyFlags::Wmi::SystemProductName
        && className == L"Win32_ComputerSystemProduct" && Same(name, L"Name"))
        replacement = &g_profile.baseBoardProduct;
    else if(PrivacyFlags::Wmi::ComputerSystemManufacturer
        && className == L"Win32_ComputerSystem" && Same(name, L"Manufacturer"))
        replacement = &g_profile.baseBoardManufacturer;
    else if(PrivacyFlags::Wmi::ComputerSystemModel
        && className == L"Win32_ComputerSystem" && Same(name, L"Model"))
        replacement = &g_profile.baseBoardProduct;
    if(replacement && ReplaceVariant(value, *replacement))
    {
        LogChange(L"WMI " + std::wstring(className) + L"." + name,
            *replacement);
    }
    VariantClear(&classValue);
    return status;
}

std::array<ExportHook, 6>& Hooks()
{
    static std::array hooks{
        ExportHook{L"advapi32.dll", "RegOpenKeyExW",
            reinterpret_cast<void*>(&HookRegOpenKeyExW),
            reinterpret_cast<void**>(&g_regOpenKeyExW)},
        ExportHook{L"advapi32.dll", "RegQueryValueExW",
            reinterpret_cast<void*>(&HookRegQueryValueExW),
            reinterpret_cast<void**>(&g_regQueryValueExW)},
        ExportHook{L"advapi32.dll", "RegGetValueW",
            reinterpret_cast<void*>(&HookRegGetValueW),
            reinterpret_cast<void**>(&g_regGetValueW)},
        ExportHook{L"advapi32.dll", "RegCloseKey",
            reinterpret_cast<void*>(&HookRegCloseKey),
            reinterpret_cast<void**>(&g_regCloseKey)},
        ExportHook{L"iphlpapi.dll", "GetAdaptersInfo",
            reinterpret_cast<void*>(&HookGetAdaptersInfo),
            reinterpret_cast<void**>(&g_getAdaptersInfo)},
        ExportHook{L"fastprox.dll", "?Get@CWbemObject@@UAGJPBGJPAUtagVARIANT@@PAJ2@Z",
            reinterpret_cast<void*>(&HookWmiGet),
            reinterpret_cast<void**>(&g_wmiGet)}
    };
    return hooks;
}

bool ReadPatch(const ExportHook& hook, std::array<BYTE, 8>& bytes)
{
    if(!hook.target)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(hook.target, &info, sizeof(info)) || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    __try
    {
        std::memcpy(bytes.data(), hook.target, bytes.size());
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Install(ExportHook& hook)
{
    HMODULE module = GetModuleHandleW(hook.module);
    if(!module)
        module = LoadLibraryW(hook.module);
    hook.target = module ? reinterpret_cast<void*>(GetProcAddress(module, hook.name))
        : nullptr;
    if(!hook.target)
        return false;
    const MH_STATUS created = MH_CreateHook(hook.target, hook.detour, hook.original);
    if(created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enabled = MH_EnableHook(hook.target);
    if(enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        return false;
    hook.installed = ReadPatch(hook, hook.patch);
    return hook.installed;
}

bool Repair(ExportHook& hook)
{
    if(!hook.installed)
        return Install(hook);
    std::array<BYTE, 8> current{};
    if(ReadPatch(hook, current) && current == hook.patch)
        return true;
    MH_DisableHook(hook.target);
    const MH_STATUS enabled = MH_EnableHook(hook.target);
    if(enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        return false;
    const bool repaired = ReadPatch(hook, hook.patch);
    if(repaired)
        Log::Write(L"[privacy] restored overwritten API hook");
    return repaired;
}

bool InstallHooks()
{
    std::call_once(g_profileOnce, &InitializePrivacyProfile);
    if(!ValidPrivacyProfile())
    {
        Log::Write(L"[privacy] generated profile failed format validation");
        return false;
    }
    std::size_t installed{};
    for(ExportHook& hook : Hooks())
        installed += Install(hook) ? 1u : 0u;
    g_installed.store(installed == Hooks().size(), std::memory_order_release);
    Log::Write(L"[privacy] in-memory registry, WMI and adapter hooks installed: "
        + std::to_wstring(installed) + L"/" + std::to_wstring(Hooks().size()));
    return installed == Hooks().size();
}

DWORD WINAPI InstallThread(void*)
{
    AcquireSRWLockExclusive(&g_installLock);
    const bool installed = InstallHooks();
    ReleaseSRWLockExclusive(&g_installLock);
    if(!installed)
        Log::Write(L"[privacy] one or more privacy hooks are unavailable");
    return 0;
}
}

void UpdatePrivacyClient(HMODULE client)
{
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if(client)
    {
        __try
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(client);
            if(dos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    reinterpret_cast<const BYTE*>(client) + dos->e_lfanew);
                if(nt->Signature == IMAGE_NT_SIGNATURE)
                {
                    begin = reinterpret_cast<std::uintptr_t>(client);
                    end = begin + nt->OptionalHeader.SizeOfImage;
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            begin = 0;
            end = 0;
        }
    }
    const std::uintptr_t oldBegin = g_clientBegin.load(std::memory_order_acquire);
    const std::uintptr_t oldEnd = g_clientEnd.load(std::memory_order_acquire);
    if(oldBegin == begin && oldEnd == end)
        return;
    g_clientBegin.store(0, std::memory_order_release);
    g_clientEnd.store(end, std::memory_order_release);
    if(oldBegin != begin)
    {
        AcquireSRWLockExclusive(&g_registryLock);
        g_registryPaths.clear();
        ReleaseSRWLockExclusive(&g_registryLock);
    }
    g_clientBegin.store(begin, std::memory_order_release);
}

bool StartPrivacyHooks(HMODULE client)
{
    UpdatePrivacyClient(client);
    if(g_started.exchange(true))
        return RepairPrivacyHooks();
    HANDLE thread = CreateThread(nullptr, 0, &InstallThread, nullptr, 0, nullptr);
    if(!thread)
    {
        g_started.store(false, std::memory_order_release);
        return false;
    }
    CloseHandle(thread);
    return true;
}

bool RepairPrivacyHooks()
{
    AcquireSRWLockExclusive(&g_installLock);
    bool repaired = true;
    for(ExportHook& hook : Hooks())
        repaired = Repair(hook) && repaired;
    g_installed.store(repaired, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_installLock);
    return repaired;
}
