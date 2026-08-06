#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>


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