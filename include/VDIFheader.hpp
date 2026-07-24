#pragma once

#include <cstdint>
#include <cstring>

class VDIFHeader
{
public:
    static constexpr int HEADER_SIZE = 32;

    VDIFHeader()
    {
        memset(data_, 0, sizeof(data_));
    }
    void setBitsPerSample(uint8_t bits)
    {
        // Word3 bits 26-30
        word(3) &= ~(0x1f << 26);

        word(3) |=
            (((bits - 1) & 0x1f) << 26);
    }
    void setTime(uint32_t seconds,
                 uint32_t frame_number,
                 uint8_t epoch)
    {
        // Word0
        word(0) = seconds;

        // Word1
        word(1) = 0;
        word(1) |= (frame_number & 0x00ffffff);
        word(1) |= ((epoch & 0x3f) << 24);
    }

    void setFrameLength(uint32_t total_bytes)
    {
        uint32_t length8 = total_bytes / 8;

        // Word2:
        // frame length in units of 8 bytes
        word(2) &= 0xff000000;
        word(2) |= (length8 & 0xffffff);
    }

    void setThread(uint16_t thread_id)
    {
        // Word3
        word(3) &= ~(0xffff << 16);

        word(3) |=
            ((uint32_t)thread_id << 16);
    }

    void setStation(uint16_t station)
    {
        // Word3 lower 16 bit

        word(3) &= 0xffff0000;

        word(3) |= station;
    }

    void setInvalid(bool invalid)
    {
        if (invalid)
            word(0) |= (1u << 31);
        else
            word(0) &= ~(1u << 31);
    }

    const uint8_t *data() const
    {
        return data_;
    }

    uint8_t *data()
    {
        return data_;
    }

private:
    uint32_t &word(int i)
    {
        return reinterpret_cast<uint32_t *>(data_)[i];
    }

private:
    alignas(4)
        uint8_t data_[HEADER_SIZE];
};