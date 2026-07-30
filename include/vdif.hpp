#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>

class VDIF {
public:
    static constexpr size_t HEADER_SIZE = 32;
    static constexpr size_t WORDS = 8;

    VDIF() { std::memset(header_, 0, HEADER_SIZE); }
    VDIF(const VDIF&) = default;
    VDIF& operator=(const VDIF&) = default;

    // copy-in 32-byte header (now treated/stored as little-endian byte order)
    void copyIn(const uint8_t *buf, size_t len) {
        if (!buf || len < HEADER_SIZE) throw std::invalid_argument("buffer too small");
        std::memcpy(header_, buf, HEADER_SIZE);
    }

    // pointer to internal 32-byte header (little-endian representation)
    const uint8_t* headerPtr() const { return header_; }
    size_t headerSize() const { return HEADER_SIZE; }

    // little-endian helpers
    static uint32_t read_le32(const uint8_t *p) {
        return (uint32_t(p[0])      ) | (uint32_t(p[1]) << 8)
             | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    static void write_le32(uint8_t *p, uint32_t v) {
        p[0] = uint8_t((v      ) & 0xFF);
        p[1] = uint8_t((v >> 8 ) & 0xFF);
        p[2] = uint8_t((v >> 16) & 0xFF);
        p[3] = uint8_t((v >> 24) & 0xFF);
    }

    // read/write 32-bit little-endian word from header_
    uint32_t readWordLE(size_t idx) const {
        if (idx >= WORDS) throw std::out_of_range("word index");
        return read_le32(header_ + idx*4);
    }
    void writeWordLE(size_t idx, uint32_t v) {
        if (idx >= WORDS) throw std::out_of_range("word index");
        write_le32(header_ + idx*4, v);
    }

    // --- Field getters/setters use little-endian word values ---
    // Word0: [secs_from_epoch:30][invalid:1 @bit30][legacy:1 @bit31]
    uint32_t getSecondsFromEpoch() const { return (readWordLE(0) & 0x3FFFFFFFu); }
    void setSecondsFromEpoch(uint32_t s) {
        uint32_t w0 = readWordLE(0);
        w0 = (w0 & ~0x3FFFFFFFu) | (s & 0x3FFFFFFFu);
        writeWordLE(0, w0);
    }
    bool isInvalid() const { return ((readWordLE(0) >> 30) & 0x1u) != 0; }
    void setInvalid(bool v) {
        uint32_t w0 = readWordLE(0);
        w0 = v ? (w0 | (1u << 30)) : (w0 & ~(1u << 30));
        writeWordLE(0, w0);
    }
    bool isLegacy() const { return ((readWordLE(0) >> 31) & 0x1u) != 0; }
    void setLegacy(bool v) {
        uint32_t w0 = readWordLE(0);
        w0 = v ? (w0 | (1u << 31)) : (w0 & ~(1u << 31));
        writeWordLE(0, w0);
    }

    // Word1: frame_number (bits0..23), epoch bits24..29
    uint32_t getFrameNumber() const { return (readWordLE(1) & 0x00FFFFFFu); }
    void setFrameNumber(uint32_t fn) {
        uint32_t w1 = readWordLE(1);
        w1 = (w1 & 0xFF000000u) | (fn & 0x00FFFFFFu);
        writeWordLE(1, w1);
    }
    uint8_t getEpoch() const { return uint8_t((readWordLE(1) >> 24) & 0x3Fu); }
    void setEpoch(uint8_t e) {
        uint32_t w1 = readWordLE(1);
        w1 = (w1 & 0x00FFFFFFu) | ((uint32_t(e & 0x3F) << 24));
        writeWordLE(1, w1);
    }

    // Word2 mapping
    uint16_t getStationID() const { return uint16_t(readWordLE(2) & 0x03FFu); }
    void setStationID(uint16_t s) {
        uint32_t w2 = readWordLE(2);
        w2 = (w2 & ~uint32_t(0x03FFu)) | (uint32_t(s & 0x03FFu));
        writeWordLE(2, w2);
    }
    uint16_t getThreadID() const { return uint16_t((readWordLE(2) >> 10) & 0x3Fu); }
    void setThreadID(uint16_t t) {
        uint32_t w2 = readWordLE(2);
        w2 = (w2 & ~(uint32_t(0x3Fu) << 10)) | ((uint32_t(t & 0x3Fu) << 10));
        writeWordLE(2, w2);
    }
    uint8_t getVersion() const { return uint8_t((readWordLE(2) >> 16) & 0x3Fu); }
    void setVersion(uint8_t v) {
        uint32_t w2 = readWordLE(2);
        w2 = (w2 & ~(uint32_t(0x3Fu) << 16)) | ((uint32_t(v & 0x3Fu) << 16));
        writeWordLE(2, w2);
    }
    size_t getHeaderLengthBytes() const { return size_t(((readWordLE(2) >> 22) & 0x03FFu) * 8); }
    void setHeaderLengthBytes(size_t bytes) {
        uint32_t units = static_cast<uint32_t>((bytes / 8) & 0x03FFu);
        uint32_t w2 = readWordLE(2);
        w2 = (w2 & ~(uint32_t(0x3FFu) << 22)) | (units << 22);
        writeWordLE(2, w2);
    }

    // Word7: noise word and LSB convenience
    uint32_t getNoiseSourceWord() const { return readWordLE(7); }
    void setNoiseSourceWord(uint32_t v) { writeWordLE(7, v); }
    bool getNoiseSourceOn() const { return (readWordLE(7) & 0x1u) != 0; }
    void setNoiseSourceOn(bool on) {
        uint32_t w7 = readWordLE(7);
        w7 = on ? (w7 | 0x1u) : (w7 & ~0x1u);
        writeWordLE(7, w7);
    }

private:
    uint8_t header_[HEADER_SIZE];
};
