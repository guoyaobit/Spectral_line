#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>
#include <arpa/inet.h> // ntohl
#include <ctime>
#include <iostream>

class VDIFPacket {
public:
    static constexpr size_t HEADER_SIZE = 32; // 32字节头部

    VDIFPacket(const uint8_t* data, size_t len) {
        if (!data || len < HEADER_SIZE) {
            throw std::runtime_error("数据长度不足以包含VDIF头部");
        }
        parseHeader(data);
        // raw_data.assign(data, data + len);
    }

    uint32_t getSecondsFromEpoch() const { return seconds_from_epoch; }
    uint32_t getFrameNumber() const { return frame_number; }
    u_int64_t getTimestamp() const {return timestamp_ns;}
    uint16_t getStationID() const { return station_id; }
    uint16_t getThreadID() const { return thread_id; }
    uint8_t  getVersion() const { return version; }
    bool     isInvalid() const { return invalid; }
    bool     isLegacy() const { return legacy; }
    size_t   getHeaderLengthBytes() const { return header_length_bytes; }

    /// 获取 UTC 时间字符串（VDIF Epoch 0 从 2000-01-01 00:00:00 开始）
    // std::string getDateTimeUTC() const {
    //     std::time_t t = seconds_from_epoch + epoch_0_offset();
    //     char buf[64];
    //     std::tm tm{};
    //     gmtime_r(&t, &tm);
    //     std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    //     return buf;
    // }

    /// 获取有效负载指针
    const uint8_t* getPayload() const {
        return raw_data.data() + header_length_bytes;
    }

    /// 获取有效负载长度
    size_t getPayloadSize() const {
        return frame_length_bytes > header_length_bytes
               ? frame_length_bytes - header_length_bytes
               : 0;
    }
    uint64_t toUnixTimestampNs(uint32_t frames_per_second) const {
        // 每个 half-year (≈6 months)
        static const time_t base_unix_2000 = 946684800; // 2000-01-01 00:00:00 UTC
        // 约等于半年 = 182.625天
        const double half_year_seconds = 182.625 * 24 * 3600.0;

        double epoch_offset = epoch * half_year_seconds;
        double t_sec = base_unix_2000 + epoch_offset + seconds_from_epoch
                     + (double)frame_number / frames_per_second;

        return static_cast<uint64_t>(t_sec * 1e9);
    }
private:
    std::vector<uint8_t> raw_data;

    uint32_t seconds_from_epoch = 0;
    bool invalid = false;
    bool legacy = false;

    uint32_t frame_number = 0;
    int epoch;
    uint32_t frame_length_bytes = 0;

    uint16_t station_id = 0;
    uint16_t thread_id = 0;
    uint8_t version = 0;
    uint32_t header_length_bytes = HEADER_SIZE;
    uint64_t timestamp_ns;
    void parseHeader(const uint8_t* data) {
        uint32_t words[8];
        std::memcpy(words, data, HEADER_SIZE);
// TODO add on/off
        uint32_t word0 = words[0];
        uint32_t word1 = words[1];
        uint32_t word2 = words[2];

        // Word 0
        seconds_from_epoch = word0 & 0x3FFFFFFF;
        invalid = (word0 >> 30) & 0x1;
        legacy  = (word0 >> 31) & 0x1;

        // Word 1
        frame_number = word1 & 0x00FFFFFF;
        epoch = (word1>>24) & 0x3F;

        // Word 2
        station_id = word2 & 0x3FF;
        thread_id  = (word2 >> 10) & 0x3F;
        version    = (word2 >> 16) & 0x3F;
        header_length_bytes = ((word2 >> 22) & 0x3FF) * 8;
        timestamp_ns = toUnixTimestampNs(62500);
    }
};
