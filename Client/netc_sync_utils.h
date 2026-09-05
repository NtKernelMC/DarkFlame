#pragma once

#include "netc_bitstream.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace NetcSyncUtil
{
struct VehicleTemplate
{
    std::vector<unsigned char> prefix;
    std::vector<unsigned char> position;
    std::vector<unsigned char> rotation;
    std::vector<unsigned char> suffix;
    unsigned int prefixBits{};
    unsigned int positionBits{};
    unsigned int rotationBits{};
    unsigned int suffixBits{};
    unsigned short version{};
    ULONGLONG tick{};
    bool valid{};
};

enum class VehicleCaptureStatus : unsigned int
{
    None,
    Ready,
    Malformed,
    CopyFailed,
    OnFoot
};

struct VehicleLayout
{
    unsigned int positionStart{};
    unsigned int positionBits{};
    unsigned int rotationStart{};
    unsigned int trailerStart{};
    unsigned int trailerEnd{};
};

enum class VehicleLayoutResult
{
    Ready,
    Malformed
};

inline const wchar_t* CaptureStatusText(VehicleCaptureStatus status)
{
    switch(status)
    {
        case VehicleCaptureStatus::Ready: return L"ready";
        case VehicleCaptureStatus::Malformed: return L"malformed";
        case VehicleCaptureStatus::CopyFailed: return L"copy-failed";
        case VehicleCaptureStatus::OnFoot: return L"on-foot";
        default: return L"none";
    }
}

inline int Round(float value)
{
    return static_cast<int>(std::floor(value + 0.5f));
}

inline void WriteMappedFloat(Netc::BitStream& stream, float value,
    unsigned int bits, float minimum, float maximum,
    bool preserveGreaterThanMinimum = false)
{
    value = std::clamp(value, minimum, maximum);
    const float alpha = (value - minimum) / (maximum - minimum);
    const unsigned int maximumBits = (1u << bits) - 1;
    unsigned int encoded = static_cast<unsigned int>(
        Round(static_cast<float>(maximumBits) * alpha));
    if(preserveGreaterThanMinimum && !encoded && alpha > 0.0f)
        encoded = 1;
    stream.WriteBits(reinterpret_cast<const char*>(&encoded), bits);
}

inline void WriteFixedPositionAxis(Netc::BitStream& stream, float value)
{
    value = std::clamp(value, -8192.0f, 8191.0f);
    const int encoded = Round(value * 1024.0f);
    stream.WriteBits(reinterpret_cast<const char*>(&encoded), 24);
}

inline bool ReadLocalPlayerState(float* position, float& health, float& armor)
{
    __try
    {
        const std::uintptr_t ped = *reinterpret_cast<const std::uint32_t*>(
            0x00B6F5F0u);
        if(!ped)
            return false;
        if(position)
        {
            position[0] = *reinterpret_cast<const float*>(ped + 0x4);
            position[1] = *reinterpret_cast<const float*>(ped + 0x8);
            position[2] = *reinterpret_cast<const float*>(ped + 0xC);
        }
        health = *reinterpret_cast<const float*>(ped + 0x540);
        armor = *reinterpret_cast<const float*>(ped + 0x548);
        return (!position || (std::isfinite(position[0])
            && std::isfinite(position[1]) && std::isfinite(position[2])))
            && std::isfinite(health) && std::isfinite(armor);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

inline bool WritePlayerPureSync(Netc::BitStream& stream, float x, float y,
    float z, float health, float armor)
{
    const unsigned char zeroByte{};
    const char zeroAxis{};
    stream.Write(zeroByte);
    stream.WriteBits(reinterpret_cast<const char*>(&zeroByte), 8);
    if(stream.Version() >= 0x06F)
    {
        stream.WriteBit(false);
        stream.WriteBit(false);
    }
    stream.Write(zeroAxis);
    stream.Write(zeroAxis);

    const unsigned short flags = 1u << 10;
    stream.WriteBits(reinterpret_cast<const char*>(&flags), 12);
    if(stream.Version() >= 0x08A)
        stream.WriteBit(false);
    WriteFixedPositionAxis(stream, x);
    WriteFixedPositionAxis(stream, y);
    z = std::clamp(z, -99999.0f, 99999.0f);
    stream.Write(z);
    WriteMappedFloat(stream, 0.0f, 16, -3.14159265f, 3.14159265f);
    stream.WriteBit(false);
    WriteMappedFloat(stream, health, 8, 0.0f, 255.0f, true);
    WriteMappedFloat(stream, armor, 8, 0.0f, 127.5f, true);
    WriteMappedFloat(stream, 0.0f, 12, -3.14159265f, 3.14159265f);
    WriteMappedFloat(stream, 0.0f, 8, -3.14159265f, 3.14159265f);
    WriteMappedFloat(stream, 0.0f, 8, -3.14159265f, 3.14159265f);
    stream.WriteBit(false);
    unsigned int cameraRange{};
    stream.WriteBits(reinterpret_cast<const char*>(&cameraRange), 2);
    WriteMappedFloat(stream, 0.0f, 3, -4.0f, 4.0f);
    WriteMappedFloat(stream, 0.0f, 3, -4.0f, 4.0f);
    WriteMappedFloat(stream, 0.0f, 3, -4.0f, 4.0f);
    stream.WriteBit(false);
    return true;
}

inline bool SkipBits(Netc::BitStream& stream, unsigned int count,
    unsigned int total)
{
    const int offset = stream.Offset();
    if(offset < 0 || static_cast<unsigned int>(offset) > total
        || count > total - static_cast<unsigned int>(offset))
    {
        return false;
    }
    stream.Offset(offset + static_cast<int>(count));
    return true;
}

inline bool ReadFlag(Netc::BitStream& stream, bool& value, unsigned int total)
{
    if(!SkipBits(stream, 0, total) || stream.GetNumberOfUnreadBits() < 1)
        return false;
    return stream.ReadBit(value);
}

inline bool SkipVelocity(Netc::BitStream& stream, unsigned int total)
{
    bool present{};
    if(!ReadFlag(stream, present, total))
        return false;
    if(!present)
        return true;
    float module{};
    float x{};
    float y{};
    float z{};
    return stream.Read(module) && stream.ReadNormVector(x, y, z)
        && SkipBits(stream, 0, total);
}

inline VehicleLayoutResult ReadVehicleLayout(Netc::BitStream& stream,
    unsigned int positionBits, VehicleLayout& layout)
{
    constexpr unsigned int ElementIdBits = 17;
    constexpr unsigned int RotationBits = 48;
    constexpr unsigned int MinimumSuffixBits = 28;
    constexpr unsigned int CameraBits[] = {3, 5, 9, 14};
    const int used = stream.GetNumberOfBitsUsed();
    if(used <= 0)
        return VehicleLayoutResult::Malformed;
    const unsigned int total = static_cast<unsigned int>(used);
    stream.Offset(0);

    unsigned char context{};
    if(!stream.Read(context) || !SkipBits(stream, 8, total))
        return VehicleLayoutResult::Malformed;
    if(stream.Version() >= 0x06F)
    {
        for(int index{}; index < 2; ++index)
        {
            bool analog{};
            if(!ReadFlag(stream, analog, total)
                || (analog && !SkipBits(stream, 8, total)))
            {
                return VehicleLayoutResult::Malformed;
            }
        }
    }
    if(!SkipBits(stream, 16, total))
        return VehicleLayoutResult::Malformed;
    if(stream.Version() >= 0x05F && !SkipBits(stream, 32, total))
        return VehicleLayoutResult::Malformed;

    layout.positionStart = static_cast<unsigned int>(stream.Offset());
    layout.positionBits = positionBits;
    if(!SkipBits(stream, positionBits + 73 + 16 + 1, total))
        return VehicleLayoutResult::Malformed;
    unsigned char cameraIndex{};
    if(!stream.ReadBits(reinterpret_cast<char*>(&cameraIndex), 2)
        || cameraIndex > 3
        || !SkipBits(stream, CameraBits[cameraIndex] * 3, total))
    {
        return VehicleLayoutResult::Malformed;
    }

    if(!SkipBits(stream, 4, total))
        return VehicleLayoutResult::Malformed;
    layout.rotationStart = static_cast<unsigned int>(stream.Offset());
    if(!SkipBits(stream, RotationBits, total)
        || !SkipVelocity(stream, total)
        || !SkipVelocity(stream, total)
        || !SkipBits(stream, 12, total))
    {
        return VehicleLayoutResult::Malformed;
    }

    layout.trailerStart = static_cast<unsigned int>(stream.Offset());
    for(unsigned int depth{}; depth < 64; ++depth)
    {
        bool hasTrailer{};
        if(!ReadFlag(stream, hasTrailer, total))
            return VehicleLayoutResult::Malformed;
        if(!hasTrailer)
        {
            layout.trailerEnd = static_cast<unsigned int>(stream.Offset());
            return layout.trailerEnd <= total
                && total - layout.trailerEnd >= MinimumSuffixBits
                ? VehicleLayoutResult::Ready : VehicleLayoutResult::Malformed;
        }
        if(!SkipBits(stream, ElementIdBits + positionBits + RotationBits, total))
            return VehicleLayoutResult::Malformed;
    }
    return VehicleLayoutResult::Malformed;
}

inline bool ReadPackedBits(Netc::BitStream& stream, unsigned int offset,
    unsigned int count, std::vector<unsigned char>& output)
{
    output.assign((count + 7) >> 3, 0);
    if(!count)
        return true;
    stream.Offset(static_cast<int>(offset));
    return stream.ReadBits(reinterpret_cast<char*>(output.data()), count);
}
}
