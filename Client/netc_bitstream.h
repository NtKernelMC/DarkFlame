#pragma once

// The virtual order is part of the netc.dll ABI and must remain unchanged.
namespace Netc
{
class BitStream;

class RefCountable
{
protected:
    virtual ~RefCountable() = default;
};

class BitStream : public RefCountable
{
public:
    virtual operator BitStream&() = 0;
    virtual int GetReadOffsetAsBits() = 0;
    virtual void SetReadOffsetAsBits(int offset) = 0;
    virtual void Reset() = 0;
    virtual void ResetReadPointer() = 0;
    virtual void Write(const unsigned char& value) = 0;
    virtual void Write(const char& value) = 0;
    virtual void Write(const unsigned short& value) = 0;
    virtual void Write(const short& value) = 0;
    virtual void Write(const unsigned int& value) = 0;
    virtual void Write(const int& value) = 0;
    virtual void Write(const float& value) = 0;
    virtual void Write(const double& value) = 0;
    virtual void Write(const char* data, int size) = 0;
    virtual void Write(const void* sync) = 0;
    virtual void WriteCompressed(const unsigned char& value) = 0;
    virtual void WriteCompressed(const char& value) = 0;
    virtual void WriteCompressed(const unsigned short& value) = 0;
    virtual void WriteCompressed(const short& value) = 0;
    virtual void WriteCompressed(const unsigned int& value) = 0;
    virtual void WriteCompressed(const int& value) = 0;
    virtual void WriteCompressed(const float& value) = 0;
    virtual void WriteCompressed(const double& value) = 0;
    virtual void WriteBits(const char* data, unsigned int count) = 0;
    virtual void WriteBit(bool value) = 0;
    virtual void WriteNormVector(float x, float y, float z) = 0;
    virtual void WriteVector(float x, float y, float z) = 0;
    virtual void WriteNormQuat(float w, float x, float y, float z) = 0;
    virtual void WriteOrthMatrix(float m00, float m01, float m02,
        float m10, float m11, float m12, float m20, float m21, float m22) = 0;
    virtual bool Read(unsigned char& value) = 0;
    virtual bool Read(char& value) = 0;
    virtual bool Read(unsigned short& value) = 0;
    virtual bool Read(short& value) = 0;
    virtual bool Read(unsigned int& value) = 0;
    virtual bool Read(int& value) = 0;
    virtual bool Read(float& value) = 0;
    virtual bool Read(double& value) = 0;
    virtual bool Read(char* data, int size) = 0;
    virtual bool Read(void* sync) = 0;
    virtual bool ReadCompressed(unsigned char& value) = 0;
    virtual bool ReadCompressed(char& value) = 0;
    virtual bool ReadCompressed(unsigned short& value) = 0;
    virtual bool ReadCompressed(short& value) = 0;
    virtual bool ReadCompressed(unsigned int& value) = 0;
    virtual bool ReadCompressed(int& value) = 0;
    virtual bool ReadCompressed(float& value) = 0;
    virtual bool ReadCompressed(double& value) = 0;
    virtual bool ReadBits(char* data, unsigned int count) = 0;
    virtual bool ReadBit() = 0;
    virtual bool ReadNormVector(float& x, float& y, float& z) = 0;
    virtual bool ReadVector(float& x, float& y, float& z) = 0;
    virtual bool ReadNormQuat(float& w, float& x, float& y, float& z) = 0;
    virtual bool ReadOrthMatrix(float& m00, float& m01, float& m02,
        float& m10, float& m11, float& m12, float& m20, float& m21, float& m22) = 0;
    virtual int GetNumberOfBitsUsed() const = 0;
    virtual int GetNumberOfBytesUsed() const = 0;
    virtual int GetNumberOfUnreadBits() const = 0;
    virtual void AlignWriteToByteBoundary() const = 0;
    virtual void AlignReadToByteBoundary() const = 0;
    virtual unsigned char* GetData() const = 0;
    virtual unsigned short Version() const = 0;

    bool ReadBit(bool& value)
    {
        unsigned char bit{};
        if(!ReadBits(reinterpret_cast<char*>(&bit), 1))
            return false;
        value = (bit & 1) != 0;
        return true;
    }

    int Offset() { return GetReadOffsetAsBits(); }
    void Offset(int value) { SetReadOffsetAsBits(value); }
    bool Bytes(char* data, int size) { return size >= 0 && Read(data, size); }
    bool Compressed(unsigned short& value) { return ReadCompressed(value); }
    bool Compressed(unsigned int& value) { return ReadCompressed(value); }
    bool Compressed(int& value) { return ReadCompressed(value); }
    bool Bits(void* data, unsigned int count)
    {
        return ReadBits(static_cast<char*>(data), count);
    }
    bool Bit(bool& value) { return ReadBit(value); }
    bool Float(float& value) { return Read(value); }
    bool Double(double& value) { return Read(value); }
    void Align() { AlignReadToByteBoundary(); }
};
}
