/**
 * FairplayKD_Connection.h — клиент для драйвера FairplayKD.sys (античит MTA / netc.dll)
 *
 * Чистая реализация из реверс-инжиниринга:
 *
 *   Устройство : \\.\FairplayKD0   (драйвер: \Device\FairplayKD0)
 *   IOCTL      : 0x22E008 (METHOD_BUFFERED)
 *   Magic      : 363 (0x16B) — второй dword каждого пакета
 *
 * Каждый пакет: [command:4][magic:4][payload...][checksum:4]
 * Checksum (sub_19200, драйвер):
 *   v = 0x04651A63;
 *   for (i = 0; i + 4 < total_size; ++i)
 *       v = (data[i] + v) ^ (data[i] << ((i & 7) + 8));
 *
 * Команды драйвера (sub_198C4 dispatch):
 *   1     — handshake, возвращает 363
 *   121   — вернуть PE imagebase ntoskrnl (MZ scan вниз по страницам)
 *   122   — MmGetSystemRoutineAddress (resolve ядерного импорта)
 *   123   — memcmp-поиск подстроки (whitelist blob)
 *   124   — write/validate (sub_1B538)
 *   125   — читать глобальный blob по частям (word_20118 bytes/запрос)
 *   126   — запросить/получить сгенерированный blob (96 байт)
 *   128..131 — checksum-only / counters
 *   132   — process hash (sub_19270)
 *   133   — validate memory ranges (ZwUnmapViewOfSection по диапазону)
 *   134..143 — дополнительные операции
 *
 * Команды клиента netc.dll (не попадают в driver switch — другая сторона):
 *   151, 152, 164 — пакеты netc-протокола (sub_101B3B60 / sub_101AE980)
 */

#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fpkd {

// ============================================================================
// Константы протокола
// ============================================================================

inline constexpr DWORD     kIoctlCode      = 0x22E008;
inline constexpr uint32_t  kMagic          = 363;      // 0x16B
inline constexpr uint32_t  kChecksumSeed   = 0x04651A63;
inline constexpr wchar_t   kDevicePath[]   = L"\\\\.\\FairplayKD0";

// Путь к игре (подтверждён существованием)
inline constexpr wchar_t   kGtaExePath[]   = L"C:\\Province Games\\bin\\gta_sa.exe";

// Образ игры (из IDA gta_sa-us-hoodlum)
inline constexpr uint32_t  kGtaImageBase   = 0x00AC0000;
inline constexpr uint32_t  kGtaImageSize   = 0x01177000;

// ============================================================================
// Структуры пакетов
// ============================================================================

#pragma pack(push, 1)

struct FpkdHeader {
    uint32_t command;
    uint32_t magic;
};

// Общий ответ на простую команду: драйвер возвращает magic
struct FpkdSimpleResponse {
    uint32_t value;
};

// case 121 — ответ драйвера
struct FpkdModuleInfo {
    uint64_t exportAddr;   // base + export dir
    uint64_t moduleSize;   // SizeOfImage
};

// case 126 — 96-байтовый блоб (заполняется драйвером)
struct FpkdBlob {
    uint32_t magic0;           // [+0]  = 363 после первого ответа
    uint16_t count;            // [+4]  счётчик (word_20118)
    uint16_t flags;            // [+6]  (word_2011A)
    uint8_t  reserved0[8];     // [+8]
    uint8_t  enabled;          // [+16] byte_22FF5
    uint8_t  reserved1[15];    // [+17]
    uint64_t selfPtr;          // [+32] указатель (sub_13634 в драйвере)
    uint32_t randomState;      // [+40] из sub_11898(6)
    uint64_t randomA[3];       // [+44] из sub_118C8(6, i)
    uint16_t randomB0;         // [+68] из sub_11898(5)
    uint8_t  randomPad[6];     // [+70]
    uint64_t randomB[3];       // [+76]
}; // всего 100 → драйвер кладёт 96 байт, храним с запасом
static_assert(sizeof(FpkdBlob) >= 96, "blob must hold 96 bytes");

#pragma pack(pop)

// ============================================================================
// Checksum — sub_19200
// ============================================================================

inline uint32_t ComputeChecksum(const uint8_t* data, size_t totalSize)
{
    if (totalSize < 4) return 0;
    uint32_t v = kChecksumSeed;
    for (size_t i = 0; i + 4 < totalSize; ++i)
        v = (data[i] + v) ^ (static_cast<uint32_t>(data[i]) << ((i & 7) + 8));
    return v;
}

// Пишет checksum в последние 4 байта пакета
inline void FinalizePacket(uint8_t* packet, size_t totalSize)
{
    *reinterpret_cast<uint32_t*>(packet + totalSize - 4) =
        ComputeChecksum(packet, totalSize);
}

// ============================================================================
// Маппер gta_sa.exe — читает игровые байты БЕЗ запуска игры
//
// Работает через CreateFileMapping: мапит файл в память, парсит PE-заголовки,
// и отдаёт байты по "игровому адресу" (как в IDA: 0xAC0000 + RVA).
//
// Использование:
//   GtaFileMapper mapper;
//   if (mapper.Open(kGtaExePath)) {
//       std::vector<uint8_t> bytes;
//       mapper.ReadBytes(0xAC1234, 256, bytes);
//   }
// ============================================================================

class GtaFileMapper {
public:
    GtaFileMapper() = default;
    ~GtaFileMapper() { Close(); }

    GtaFileMapper(const GtaFileMapper&) = delete;
    GtaFileMapper& operator=(const GtaFileMapper&) = delete;

    bool Open(const wchar_t* exePath)
    {
        Close();

        m_file = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            m_lastError = GetLastError();
            return false;
        }

        m_map = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m_map) { m_lastError = GetLastError(); Close(); return false; }

        m_base = static_cast<const uint8_t*>(MapViewOfFile(m_map, FILE_MAP_READ, 0, 0, 0));
        if (!m_base) { m_lastError = GetLastError(); Close(); return false; }

        // --- парсим PE ---
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m_base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { m_lastError = ERROR_BAD_EXE_FORMAT; Close(); return false; }

        m_fileSize = GetFileSize(m_file, nullptr);
        if (static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > m_fileSize) {
            m_lastError = ERROR_BAD_EXE_FORMAT; Close(); return false;
        }

        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            m_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) { m_lastError = ERROR_BAD_EXE_FORMAT; Close(); return false; }

        m_imageBase = nt->OptionalHeader.ImageBase;
        m_sizeOfImage = nt->OptionalHeader.SizeOfImage;

        m_sections = IMAGE_FIRST_SECTION(const_cast<IMAGE_NT_HEADERS32*>(nt));
        m_sectionCount = nt->FileHeader.NumberOfSections;

        return true;
    }

    void Close()
    {
        if (m_base) { UnmapViewOfFile(m_base); m_base = nullptr; }
        if (m_map)  { CloseHandle(m_map); m_map = nullptr; }
        if (m_file != INVALID_HANDLE_VALUE) { CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE; }
    }

    bool IsOpen() const { return m_base != nullptr; }
    DWORD GetLastError() const { return m_lastError; }

    uint32_t ImageBase() const { return m_imageBase; }
    uint32_t SizeOfImage() const { return m_sizeOfImage; }

    // Конвертирует игровой адрес (ImageBase + RVA) в file offset
    bool RvaToFileOffset(uint32_t gameAddress, uint32_t* fileOffset) const
    {
        if (!m_base) return false;
        if (gameAddress < m_imageBase) return false;
        uint32_t rva = gameAddress - m_imageBase;

        for (WORD i = 0; i < m_sectionCount; ++i)
        {
            const auto& s = m_sections[i];
            uint32_t span = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span)
            {
                uint32_t off = s.PointerToRawData + (rva - s.VirtualAddress);
                if (off >= m_fileSize) return false;
                *fileOffset = off;
                return true;
            }
        }
        return false;
    }

    // Читает байты по игровому адресу
    bool ReadBytes(uint32_t gameAddress, size_t size, std::vector<uint8_t>& out) const
    {
        out.clear();
        uint32_t off = 0;
        if (!RvaToFileOffset(gameAddress, &off)) return false;
        if (off + size > m_fileSize)
            size = m_fileSize - off;
        out.assign(m_base + off, m_base + off + size);
        return true;
    }

    struct SectionInfo { char name[9]; uint32_t rva; uint32_t vsize; uint32_t rawOff; uint32_t rawSize; };

    std::vector<SectionInfo> Sections() const
    {
        std::vector<SectionInfo> list;
        for (WORD i = 0; i < m_sectionCount; ++i)
        {
            SectionInfo si{};
            memcpy(si.name, m_sections[i].Name, 8);
            si.name[8] = 0;
            si.rva = m_sections[i].VirtualAddress;
            si.vsize = m_sections[i].Misc.VirtualSize;
            si.rawOff = m_sections[i].PointerToRawData;
            si.rawSize = m_sections[i].SizeOfRawData;
            list.push_back(si);
        }
        return list;
    }

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_map = nullptr;
    const uint8_t* m_base = nullptr;
    const IMAGE_SECTION_HEADER* m_sections = nullptr;
    WORD m_sectionCount = 0;
    DWORD m_fileSize = 0;
    DWORD m_lastError = 0;
    uint32_t m_imageBase = kGtaImageBase;
    uint32_t m_sizeOfImage = kGtaImageSize;
};

// ============================================================================
// Клиент драйвера
// ============================================================================

class FairplayKd {
public:
    FairplayKd() = default;
    ~FairplayKd() { Close(); }

    FairplayKd(const FairplayKd&) = delete;
    FairplayKd& operator=(const FairplayKd&) = delete;

    bool Open()
    {
        if (m_device != INVALID_HANDLE_VALUE) return true;
        m_device = CreateFileW(kDevicePath, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_device == INVALID_HANDLE_VALUE) {
            m_lastError = GetLastError();
            return false;
        }
        return true;
    }

    void Close()
    {
        if (m_device != INVALID_HANDLE_VALUE) {
            CloseHandle(m_device);
            m_device = INVALID_HANDLE_VALUE;
        }
    }

    bool IsOpen() const { return m_device != INVALID_HANDLE_VALUE; }
    DWORD GetLastError() const { return m_lastError; }

    // Базовое I/O. payloadSize включает 8-байтовый header + payload + 4 checksum.
    bool Ioctl(const void* inData, DWORD inSize,
               void* outData, DWORD outSize,
               DWORD* bytesReturned = nullptr)
    {
        if (!Open()) return false;
        DWORD br = 0;
        if (!DeviceIoControl(m_device, kIoctlCode,
                             const_cast<void*>(inData), inSize,
                             outData, outSize, &br, nullptr)) {
            m_lastError = GetLastError();
            return false;
        }
        if (bytesReturned) *bytesReturned = br;
        return true;
    }

    // ---- case 1: handshake -----------------------------------------------
    bool Handshake()
    {
        uint8_t req[12] = {};
        reinterpret_cast<FpkdHeader*>(req)->command = 1;
        reinterpret_cast<FpkdHeader*>(req)->magic = kMagic;
        FinalizePacket(req, sizeof(req));

        FpkdSimpleResponse resp{};
        if (!Ioctl(req, sizeof(req), &resp, sizeof(resp))) return false;
        return resp.value == kMagic;
    }

    // ---- case 121: ntoskrnl info -----------------------------------------
    bool GetNtoskrnlInfo(FpkdModuleInfo* out)
    {
        uint8_t req[16] = {};
        auto* h = reinterpret_cast<FpkdHeader*>(req);
        h->command = 121;
        h->magic = kMagic;
        *reinterpret_cast<uint32_t*>(req + 8) = 1;   // a1[2] == 1
        FinalizePacket(req, sizeof(req));

        return Ioctl(req, sizeof(req), out, sizeof(*out));
    }

    // ---- case 126: получить блоб ------------------------------------------
    bool QueryBlob(FpkdBlob* out)
    {
        uint8_t req[12] = {};
        auto* h = reinterpret_cast<FpkdHeader*>(req);
        h->command = 126;
        h->magic = kMagic;
        FinalizePacket(req, sizeof(req));

        DWORD br = 0;
        if (!Ioctl(req, sizeof(req), out, 96, &br)) return false;
        return br >= 96;
    }

    // ---- case 133: валидация диапазонов адресов игры ---------------------
    // Драйвер требует: a1[2], a1[3] — [start, end) выровнены по 4, <= 0xFDE8
    bool ValidateMemoryRange(uint32_t start, uint32_t end)
    {
        if (end < start) return false;
        uint8_t req[24] = {};
        auto* h = reinterpret_cast<FpkdHeader*>(req);
        h->command = 133;
        h->magic = kMagic;
        *reinterpret_cast<uint32_t*>(req + 8)  = start;
        *reinterpret_cast<uint32_t*>(req + 12) = end;
        FinalizePacket(req, sizeof(req));

        uint32_t result = 0;
        if (!Ioctl(req, sizeof(req), &result, sizeof(result))) return false;
        return true;
    }

    // ---- зачистка: close handle → драйвер снимает callback-и --------------
    bool Unregister()
    {
        Close();
        return true;
    }

private:
    HANDLE m_device = INVALID_HANDLE_VALUE;
    DWORD m_lastError = 0;
};

// ============================================================================
// Утилиты
// ============================================================================

inline std::string LastErrorString(DWORD code)
{
    char buf[512];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, sizeof(buf), nullptr);
    return buf;
}

} // namespace fpkd
