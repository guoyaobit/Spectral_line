#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>

class VDIF {
public:
    static constexpr size_t HEADER_SIZE = 32;
    static constexpr size_t WORDS = 8;

    // default: zero header
    VDIF() { std::memset(header_, 0, HEADER_SIZE); }
    VDIF(const VDIF&) = default;            // copies header_ automatically
    VDIF& operator=(const VDIF&) = default; // copies header_ automatically

    // copy-in 32-byte header from network/source
    void copyIn(const uint8_t *buf, size_t len) {
        if (!buf || len < HEADER_SIZE) throw std::invalid_argument("buffer too small");
        std::memcpy(header_, buf, HEADER_SIZE);
    }

    // get pointer to internal 32-byte header (big-endian). Caller may write it to file.
    const uint8_t* headerPtr() const { return header_; }
    size_t headerSize() const { return HEADER_SIZE; }

    // direct word access (index 0..7). Returns host-order uint32_t.
    uint32_t getWord(size_t idx) const {
        if (idx >= WORDS) throw std::out_of_range("word index");
        const uint8_t *p = header_ + idx * 4;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    void setWord(size_t idx, uint32_t v) {
        if (idx >= WORDS) throw std::out_of_range("word index");
        uint8_t *p = header_ + idx * 4;
        p[0] = uint8_t((v >> 24) & 0xFF);
        p[1] = uint8_t((v >> 16) & 0xFF);
        p[2] = uint8_t((v >> 8) & 0xFF);
        p[3] = uint8_t(v & 0xFF);
    }

    // --- Common field getters / setters (mapping per VDIF MSB->LSB) ---
    // word0: [secs_inre:30][legacy:1][invalid:1]
    uint32_t getSecsInre() const { return (getWord(0) >> 2) & 0x3FFFFFFFu; }
    void setSecsInre(uint32_t v) {
        uint32_t w0 = getWord(0);
        w0 = (w0 & 0x00000003u) | ((v & 0x3FFFFFFFu) << 2);
        setWord(0, w0);
    }
    bool getLegacy() const { return ((getWord(0) >> 1) & 0x1u) != 0; }
    void setLegacy(bool b) { uint32_t w0 = getWord(0); w0 = b ? (w0 | (1u << 1)) : (w0 & ~(1u << 1)); setWord(0, w0); }
    bool getInvalid() const { return (getWord(0) & 0x1u) != 0; }
    void setInvalid(bool b) { uint32_t w0 = getWord(0); w0 = b ? (w0 | 0x1u) : (w0 & ~0x1u); setWord(0, w0); }

    // word1: [df_num_insec:24][ref_epoch:6][UA:2]
    uint32_t getDfNumInSec() const { return (getWord(1) >> 8) & 0x00FFFFFFu; }
    void setDfNumInSec(uint32_t v) { uint32_t w1 = getWord(1); w1 = (w1 & 0x000000FFu) | ((v & 0x00FFFFFFu) << 8); setWord(1, w1); }
    uint8_t getRefEpoch() const { return uint8_t((getWord(1) >> 2) & 0x3Fu); }
    void setRefEpoch(uint8_t e) { uint32_t w1 = getWord(1); w1 = (w1 & ~uint32_t(0x000000FCu)) | ((uint32_t(e & 0x3F) << 2)); setWord(1, w1); }
    uint8_t getUA() const { return uint8_t(getWord(1) & 0x3u); }
    void setUA(uint8_t ua) { uint32_t w1 = getWord(1); w1 = (w1 & ~uint32_t(0x3u)) | (ua & 0x3u); setWord(1, w1); }

    // word2: [df_len:24][num_channels:5][ver:3]
    uint32_t getDfLen() const { return (getWord(2) >> 8) & 0x00FFFFFFu; }
    void setDfLen(uint32_t v) { uint32_t w2 = getWord(2); w2 = (w2 & 0x000000FFu) | ((v & 0x00FFFFFFu) << 8); setWord(2, w2); }
    uint8_t getNumChannels() const { return uint8_t((getWord(2) >> 3) & 0x1Fu); }
    void setNumChannels(uint8_t n) { uint32_t w2 = getWord(2); w2 = (w2 & ~uint32_t(0x000000F8u)) | ((uint32_t(n & 0x1F) << 3)); setWord(2, w2); }
    uint8_t getVer() const { return uint8_t(getWord(2) & 0x7u); }
    void setVer(uint8_t v) { uint32_t w2 = getWord(2); w2 = (w2 & ~uint32_t(0x7u)) | (v & 0x7u); setWord(2, w2); }

    // word3: [stationID:16][threadID:10][bps:5][dt:1]
    uint16_t getStationID() const { return uint16_t((getWord(3) >> 16) & 0xFFFFu); }
    void setStationID(uint16_t s) { uint32_t w3 = getWord(3); w3 = (w3 & 0x0000FFFFu) | (uint32_t(s & 0xFFFFu) << 16); setWord(3, w3); }
    uint16_t getThreadID() const { return uint16_t((getWord(3) >> 6) & 0x03FFu); }
    void setThreadID(uint16_t t) { uint32_t w3 = getWord(3); w3 = (w3 & ~(uint32_t(0x03FFu) << 6)) | ((uint32_t(t & 0x03FFu) << 6)); setWord(3, w3); }
    uint8_t getBps() const { return uint8_t((getWord(3) >> 1) & 0x1Fu); }
    void setBps(uint8_t b) { uint32_t w3 = getWord(3); w3 = (w3 & ~(uint32_t(0x1Fu) << 1)) | ((uint32_t(b & 0x1Fu) << 1)); setWord(3, w3); }
    bool getDt() const { return (getWord(3) & 0x1u) != 0; }
    void setDt(bool d) { uint32_t w3 = getWord(3); w3 = d ? (w3 | 0x1u) : (w3 & ~0x1u); setWord(3, w3); }

    // word4: [eud5:24][edv5:8]
    uint32_t getEud5() const { return (getWord(4) >> 8) & 0x00FFFFFFu; }
    void setEud5(uint32_t v) { uint32_t w4 = getWord(4); w4 = (w4 & 0x000000FFu) | ((v & 0x00FFFFFFu) << 8); setWord(4, w4); }
    uint8_t getEdv5() const { return uint8_t(getWord(4) & 0xFFu); }
    void setEdv5(uint8_t v) { uint32_t w4 = getWord(4); w4 = (w4 & ~uint32_t(0xFFu)) | (v & 0xFFu); setWord(4, w4); }

    // word5: PICstatus
    uint32_t getPICstatus() const { return getWord(5); }
    void setPICstatus(uint32_t v) { setWord(5, v); }

    // word6+word7: edh_psn (64-bit)
    uint64_t getEdhPsn() const { return (uint64_t(getWord(6)) << 32) | uint64_t(getWord(7)); }
    void setEdhPsn(uint64_t v) { setWord(6, uint32_t((v >> 32) & 0xFFFFFFFFu)); setWord(7, uint32_t(v & 0xFFFFFFFFu)); }

    // Noise source state stored in word7 LSB
    bool getNoiseSourceState() const { return (getWord(7) & 0x1u) != 0; }
    void setNoiseSourceState(bool on) { uint32_t w7 = getWord(7); w7 = on ? (w7 | 0x1u) : (w7 & ~0x1u); setWord(7, w7); }

private:
    uint8_t header_[HEADER_SIZE];
};