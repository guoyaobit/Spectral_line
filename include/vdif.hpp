#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>

/*
 * VDIF epoch -> Unix timestamp (seconds)
 *
 * epoch 0:
 *   2000-01-01 00:00:00 UTC
 *
 * epoch increases every 6 months
 */
static constexpr uint64_t vdif_epoch_unix_sec[64] =
{
    946684800ULL,   //  0: 2000-01-01
    962409600ULL,   //  1: 2000-07-01
    978307200ULL,   //  2: 2001-01-01
    993945600ULL,   //  3: 2001-07-01

    1009843200ULL,  //  4: 2002-01-01
    1025481600ULL,  //  5: 2002-07-01
    1041379200ULL,  //  6: 2003-01-01
    1057017600ULL,  //  7: 2003-07-01

    1072915200ULL,  //  8: 2004-01-01
    1088640000ULL,  //  9: 2004-07-01
    1104537600ULL,  // 10: 2005-01-01
    1120176000ULL,  // 11: 2005-07-01

    1136073600ULL,  // 12: 2006-01-01
    1151712000ULL,  // 13: 2006-07-01
    1167609600ULL,  // 14: 2007-01-01
    1183248000ULL,  // 15: 2007-07-01

    1199145600ULL,  // 16: 2008-01-01
    1214866800ULL,  // 17: 2008-07-01
    1230768000ULL,  // 18: 2009-01-01
    1246406400ULL,  // 19: 2009-07-01

    1262304000ULL,  // 20: 2010-01-01
    1277942400ULL,  // 21: 2010-07-01
    1293840000ULL,  // 22: 2011-01-01
    1309478400ULL,  // 23: 2011-07-01

    1325376000ULL,  // 24: 2012-01-01
    1341100800ULL,  // 25: 2012-07-01
    1356998400ULL,  // 26: 2013-01-01
    1372636800ULL,  // 27: 2013-07-01

    1388534400ULL,  // 28: 2014-01-01
    1404172800ULL,  // 29: 2014-07-01
    1420070400ULL,  // 30: 2015-01-01
    1435708800ULL,  // 31: 2015-07-01

    1451606400ULL,  // 32: 2016-01-01
    1467331200ULL,  // 33: 2016-07-01
    1483228800ULL,  // 34: 2017-01-01
    1498867200ULL,  // 35: 2017-07-01

    1514764800ULL,  // 36: 2018-01-01
    1530403200ULL,  // 37: 2018-07-01
    1546300800ULL,  // 38: 2019-01-01
    1561939200ULL,  // 39: 2019-07-01

    1577836800ULL,  // 40: 2020-01-01
    1593561600ULL,  // 41: 2020-07-01
    1609459200ULL,  // 42: 2021-01-01
    1625097600ULL,  // 43: 2021-07-01

    1640995200ULL,  // 44: 2022-01-01
    1656633600ULL,  // 45: 2022-07-01
    1672531200ULL,  // 46: 2023-01-01
    1688169600ULL,  // 47: 2023-07-01

    1704067200ULL,  // 48: 2024-01-01
    1719792000ULL,  // 49: 2024-07-01
    1735689600ULL,  // 50: 2025-01-01
    1751328000ULL,  // 51: 2025-07-01

    1767225600ULL,  // 52: 2026-01-01
    1782864000ULL,  // 53: 2026-07-01
    1798761600ULL,  // 54: 2027-01-01
    1814400000ULL,  // 55: 2027-07-01

    1830297600ULL,  // 56: 2028-01-01
    1846022400ULL,  // 57: 2028-07-01
    1861920000ULL,  // 58: 2029-01-01
    1877558400ULL,  // 59: 2029-07-01

    1893456000ULL,  // 60: 2030-01-01
    1909094400ULL,  // 61: 2030-07-01
    1924992000ULL,  // 62: 2031-01-01
    1940630400ULL   // 63: 2031-07-01
};


inline uint64_t vdif_epoch_to_unix_sec(uint8_t epoch)
{
    if (epoch >= 64)
        throw std::out_of_range("invalid VDIF epoch");

    return vdif_epoch_unix_sec[epoch];
}
inline uint64_t vdif_to_timestamp_ns(
        uint8_t epoch,
        uint32_t seconds,
        uint32_t frame)
{
    constexpr uint64_t NS_PER_SEC = 1000000000ULL;
    constexpr uint64_t FRAME_NS = 16000ULL; // 62500 frame/s
    uint64_t sec =
        vdif_epoch_to_unix_sec(epoch)
        + seconds;
    return sec * NS_PER_SEC
           + uint64_t(frame) * FRAME_NS;
}

class VDIF
{
public:

    static constexpr size_t HEADER_SIZE = 32;
    static constexpr size_t WORDS = 8;

    VDIF()
    {
        std::memset(header_,0,HEADER_SIZE);
    }

    // Copy complete 32-byte VDIF header into internal buffer
    void copyIn(const uint8_t *buf, size_t len)
    {
        if (!buf || len < HEADER_SIZE)
            throw std::invalid_argument("buffer too small");

        std::memcpy(header_, buf, HEADER_SIZE);
    }

    // Copy header to external buffer
    void copyOut(uint8_t *buf, size_t len) const
    {
        if (!buf || len < HEADER_SIZE)
            throw std::invalid_argument("buffer too small");

        std::memcpy(buf, header_, HEADER_SIZE);
    }
    void clear()
    {
        std::memset(header_,0,HEADER_SIZE);
    }

    const uint8_t* headerPtr() const
    {
        return header_;
    }

    uint8_t* headerPtr()
    {
        return header_;
    }

    // =========================
    // endian
    // =========================

    static uint32_t readLE32(const uint8_t *p)
    {
        return
            ((uint32_t)p[0]) |
            ((uint32_t)p[1]<<8) |
            ((uint32_t)p[2]<<16) |
            ((uint32_t)p[3]<<24);
    }

    static void writeLE32(uint8_t *p,uint32_t v)
    {
        p[0]=(v)&0xff;
        p[1]=(v>>8)&0xff;
        p[2]=(v>>16)&0xff;
        p[3]=(v>>24)&0xff;
    }

    uint32_t getWord(size_t index) const
    {
        if(index>=WORDS)
            throw std::out_of_range("word");

        return readLE32(header_+index*4);
    }

    void setWord(size_t index,uint32_t value)
    {
        if(index>=WORDS)
            throw std::out_of_range("word");

        writeLE32(header_+index*4,value);
    }

// =================================================
// Word 0
// Invalid / Legacy / Seconds from reference epoch
// =================================================

    uint32_t getSecondsFromEpoch() const
    {
        return getWord(0)&0x3fffffff;
    }

    void setSecondsFromEpoch(uint32_t sec)
    {
        uint32_t w=getWord(0);

        w &= 0xc0000000;
        w |= sec&0x3fffffff;

        setWord(0,w);
    }

    bool getInvalid() const
    {
        return (getWord(0)>>30)&1;
    }

    void setInvalid(bool v)
    {
        uint32_t w=getWord(0);

        if(v)
            w|=(1u<<30);
        else
            w&=~(1u<<30);

        setWord(0,w);
    }

    bool getLegacy() const
    {
        return (getWord(0)>>31)&1;
    }

    void setLegacy(bool v)
    {
        uint32_t w=getWord(0);

        if(v)
            w|=(1u<<31);
        else
            w&=~(1u<<31);

        setWord(0,w);
    }

// =================================================
// Word 1
// Reference Epoch / Frame Number
// =================================================
    uint32_t getFrameNumber() const
    {
        return getWord(1)&0xffffff;
    }

    void setFrameNumber(uint32_t n)
    {
        uint32_t w=getWord(1);

        w &=0xff000000;
        w |= n&0xffffff;

        setWord(1,w);
    }

    uint8_t getReferenceEpoch() const
    {
        return (getWord(1)>>24)&0x3f;
    }

    void setReferenceEpoch(uint8_t e)
    {
        uint32_t w=getWord(1);

        w &=0x00ffffff;
        w |=((uint32_t)(e&0x3f)<<24);

        setWord(1,w);
    }

// =================================================
// Word 2
// VDIF Version / Channels / Frame Length
// =================================================
    uint8_t getVDIFVersion() const
    {
        return (getWord(2)>>29)&0x7;
    }

    void setVDIFVersion(uint8_t v)
    {
        uint32_t w=getWord(2);

        w &= ~(0x7u<<29);
        w |= ((uint32_t)(v&0x7)<<29);

        setWord(2,w);
    }

    uint8_t getLog2Channels() const
    {
        return (getWord(2)>>24)&0x1f;
    }

    void setLog2Channels(uint8_t v)
    {
        uint32_t w=getWord(2);

        w &= ~(0x1fu<<24);
        w |= ((uint32_t)(v&0x1f)<<24);

        setWord(2,w);
    }

    uint32_t getFrameLength() const
    {
        return (getWord(2)&0xffffff)*8;
    }

    void setFrameLength(uint32_t bytes)
    {
        uint32_t w=getWord(2);

        w &=0xff000000;
        w |= (bytes/8)&0xffffff;

        setWord(2,w);
    }

// =================================================
// Word 3
// Complex / Bits / Thread / Station
// =================================================

    bool getComplex() const
    {
        return (getWord(3)>>31)&1;
    }

    void setComplex(bool v)
    {
        uint32_t w=getWord(3);

        if(v)
            w|=(1u<<31);
        else
            w&=~(1u<<31);

        setWord(3,w);
    }

    uint8_t getBitsPerSample() const
    {
        return ((getWord(3)>>26)&0x1f)+1;
    }

    void setBitsPerSample(uint8_t bits)
    {
        uint32_t w=getWord(3);

        w &= ~(0x1fu<<26);
        w |= ((uint32_t)(bits-1)<<26);

        setWord(3,w);
    }

    uint16_t getThreadID() const
    {
        return (getWord(3)>>16)&0x3ff;
    }

    void setThreadID(uint16_t id)
    {
        uint32_t w=getWord(3);

        w &= ~(0x3ffu<<16);
        w |= ((uint32_t)(id&0x3ff)<<16);

        setWord(3,w);
    }

    uint16_t getStationID() const
    {
        return getWord(3)&0xffff;
    }

    void setStationID(uint16_t id)
    {
        uint32_t w=getWord(3);

        w &=0xffff0000;
        w |=id;

        setWord(3,w);
    }

// =================================================
// Word4
// EDV + Extended User Data
// =================================================
    uint8_t getEDV() const
    {
        return getWord(4)>>24;
    }

    void setEDV(uint8_t v)
    {
        uint32_t w=getWord(4);

        w &=0x00ffffff;
        w |=((uint32_t)v<<24);

        setWord(4,w);
    }

    uint32_t getExtendedUserData() const
    {
        return getWord(4)&0xffffff;
    }

    void setExtendedUserData(uint32_t v)
    {
        uint32_t w=getWord(4);

        w &=0xff000000;
        w |=v&0xffffff;

        setWord(4,w);
    }

// =================================================
// Word5-7 Extended User Data
// =================================================

    uint32_t getExtendedData(size_t word) const
    {
        if(word<5 || word>7)
            throw std::out_of_range("extended word");

        return getWord(word);
    }

    void setExtendedData(size_t word,uint32_t value)
    {
        if(word<5 || word>7)
            throw std::out_of_range("extended word");

        setWord(word,value);
    }

// =================================================
// Noise source state
//
// EDV = 1
// Word5 bit0
// =================================================
    bool getNoiseSourceState() const
    {
        return (getWord(5)&0x1)!=0;
    }

    void setNoiseSourceState(bool on)
    {
        uint32_t w=getWord(5);

        if(on)
            w|=1;
        else
            w&=~1u;

        setWord(5,w);

        // indicate extended data is valid
        setEDV(1);
    }

private:

    uint8_t header_[HEADER_SIZE];

};