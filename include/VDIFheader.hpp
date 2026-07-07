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

    /*
     * frame_length:
     * VDIF frame total size / 8 bytes
     *
     * Example:
     * header 32B + payload 8192B
     *
     * total = 8224B
     * length = 8224 / 8 = 1028
     */
    void setFrameLength(uint32_t total_bytes,
                        uint8_t bits_per_sample)
    {
        uint32_t length8 =
            total_bytes / 8;

        // Word2
        word(2) = 0;

        // frame length: 24bit
        word(2) |= (length8 & 0xffffff);

        // bits per sample stored as bits-1
        word(2) |=
            (((bits_per_sample - 1) & 0x1f) << 26);
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