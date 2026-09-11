#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <string>
#include <filesystem>
#include <fcntl.h>  // open, O_WRONLY, O_CREAT, O_TRUNC
#include <unistd.h> // write, close, read
#include <sys/stat.h>
#include <sys/types.h>
#include <immintrin.h>
#include <Globalcfg.hpp>
#include <sys/uio.h>
#include <unistd.h>
// Ensure PacketBatch, Packet, and moodycamel queue headers are included in build

class baseband
{
private:
    int m_stream_id;
    int m_subband_id;
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queue;
    // moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueB;
    int fd = -1;
    // int fd_y = -1;
    static constexpr uint64_t DEFAULT_MAX_FILE_SIZE = 8ULL * 1024 * 1024 * 1024; // 4 GB
    uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE;
    uint64_t current_size = 0;
    uint32_t file_index = 0;
    std::string m_folder;

    static constexpr size_t FRAME_PAYLOAD = 8192;
    uint8_t vdif_payloadA[FRAME_PAYLOAD];
    // uint8_t vdif_payloadB[FRAME_PAYLOAD];
    static constexpr size_t HEADER_SIZE = 32;
    std::array<iovec, 1024> iov;

    // create_file: returns 0 on success, -1 on error
    int create_file()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
        // if (fd_y >= 0)
        // {
        //     ::close(fd);
        //     fd = -1;
        // }
        current_size = 0;

        std::filesystem::path folder(m_folder);
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        auto &cfg = GlobalConfig::getInstance();
        if (ec)
        {
            auto &cfg = GlobalConfig::getInstance();
            cfg.logger_->error("baseband: create_directories({}) failed: {}", folder.string(), ec.message());
            return -1;
        }
        // const auto &subband = *cfg.subbands[m_subband_id];

        char basename[256];
        // char basename_y[256];

        const unsigned int current_file_index = file_index++;
        const char *pol = (m_stream_id % 2 == 0) ? "X" : "Y";

        const time_t now = static_cast<time_t>(std::time(nullptr));

        struct tm utc_tm {};
        gmtime_r(&now, &utc_tm);

        char utc_date[16];
        char utc_time[16];

        std::strftime(utc_date, sizeof(utc_date),
                    "%Y-%m-%d", &utc_tm);

        std::strftime(utc_time, sizeof(utc_time),
                    "%H:%M:%S", &utc_tm);

        if (std::snprintf(
                basename,
                sizeof(basename),
                "%s_%s_%s_b%02u_%s_t%04u.vdif",
                cfg.receiver_name.c_str(),
                utc_date,
                utc_time,
                static_cast<unsigned int>(m_subband_id+cfg.ServerID*8),
                pol,
                current_file_index) < 0)
        {
            cfg.logger_->error(
                "baseband: snprintf failed for {} polarization filename",
                pol);
            return -1;
        }


        // if (std::snprintf(basename_y,
        //                 sizeof(basename_y),
        //                 "%uM-%uM_Y_%04u.vdif",
        //                 static_cast<unsigned int>(subband.start_freq / 1e6f),
        //                 static_cast<unsigned int>(subband.end_freq / 1e6f),
        //                 current_file_index) < 0)
        // {
        //     GlobalConfig::getInstance().logger_->error(
        //         "baseband: snprintf failed for Y polarization filename");
        //     return -1;
        // }

        std::filesystem::path filepath = folder / basename;
        fd = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
        {
            auto &cfg = GlobalConfig::getInstance();
            cfg.logger_->error("baseband: open('{}') failed: {} (errno={})", filepath.string(), std::strerror(errno), errno);
            return -1;
        }
        // filepath = folder / basename_y;
        // fd_y = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        // if (fd_y < 0)
        // {
        //     auto &cfg = GlobalConfig::getInstance();
        //     cfg.logger_->error("baseband: open('{}') failed: {} (errno={})", filepath.string(), std::strerror(errno), errno);
        //     return -1;
        // }
        return 0;
    }

public:
    baseband(int);
    ~baseband();
    __attribute__((target("avx512f,avx512bw"))) inline void quantize4_avx512(
        uint8_t *buf,
        size_t bytes)
    {
        size_t in = 0;
        size_t out = 0;

        alignas(64)
            uint8_t tmp[64];

        while (in + 64 <= bytes)
        {

            __m512i x =
                _mm512_loadu_si512(
                    (void *)(buf + in));

            /*
             * signed shift
             *
             * high nibble
             */

            __m512i q =
                _mm512_srai_epi16(
                    x,
                    4);

            q =
                _mm512_and_si512(
                    q,
                    _mm512_set1_epi8(0x0f));

            _mm512_store_si512(
                (__m512i *)tmp,
                q);

            for (int i = 0; i < 64; i += 2)
            {
                tmp[i / 2] =
                    (tmp[i] << 4) |
                    tmp[i + 1];
            }

            memcpy(
                buf + out,
                tmp,
                32);

            in += 64;
            out += 32;
        }

        while (in + 1 < bytes)
        {
            buf[out++] =
                ((buf[in] >> 4) << 4) |
                (buf[in + 1] >> 4);

            in += 2;
        }
    }
    // Pack the two most-significant bits of four 8-bit samples into one byte.
    // The resulting byte order is sample 0 in bits 7:6 through sample 3 in
    // bits 1:0, matching the high-bit truncation used by the 4-bit path.
    inline void quantize2(uint8_t *buf, size_t bytes)
    {
        size_t in = 0;
        size_t out = 0;

        while (in + 4 <= bytes)
        {
            buf[out++] = static_cast<uint8_t>(
                (buf[in] & 0xC0) |
                ((buf[in + 1] & 0xC0) >> 2) |
                ((buf[in + 2] & 0xC0) >> 4) |
                ((buf[in + 3] & 0xC0) >> 6));
            in += 4;
        }
    }
    void recoder()
    {
        auto &cfg = GlobalConfig::getInstance();
        // std::cout<<cfg.Baseband_bits<<std::endl;
        PacketBatch *readblock = nullptr;
        m_queue->wait_dequeue(readblock);
        const int frames_to_write = readblock->count;
        if (current_size >= max_file_size)
        {
            if (create_file() != 0)
            {
                cfg.logger_->error(
                    "baseband: create_file failed during rotation");
                return;
            }
        }
        if(cfg.Baseband_bits == 8)
        {
            for (int i = 0; i < frames_to_write; ++i)
            {       
                iov[2 * i].iov_base = &readblock->hdrs[i];
                iov[2 * i].iov_len  = HEADER_SIZE;
                iov[2 * i + 1].iov_base = readblock->pkts[i]->payload;
                iov[2 * i + 1].iov_len  = FRAME_PAYLOAD;
            }
        }
        else if(cfg.Baseband_bits == 4)
        {
            for (int i = 0; i < frames_to_write; ++i)
            {
                quantize4_avx512(readblock->pkts[i]->payload, FRAME_PAYLOAD);

                iov[2 * i].iov_base = &readblock->hdrs[i];
                iov[2 * i].iov_len  = HEADER_SIZE;

                iov[2 * i + 1].iov_base = readblock->pkts[i]->payload;
                iov[2 * i + 1].iov_len  = FRAME_PAYLOAD/2;
            }
        }
        else if(cfg.Baseband_bits == 2)
        {
            for (int i = 0; i < frames_to_write; ++i)
            {
                quantize2(readblock->pkts[i]->payload, FRAME_PAYLOAD);

                iov[2 * i].iov_base = &readblock->hdrs[i];
                iov[2 * i].iov_len  = HEADER_SIZE;

                iov[2 * i + 1].iov_base = readblock->pkts[i]->payload;
                iov[2 * i + 1].iov_len  = FRAME_PAYLOAD/4;
            }
        }
        else
        {
            cfg.logger_->error("baseband: unsupported Baseband_bits {}", cfg.Baseband_bits);
            return;
        }
        const int iov_count = frames_to_write * 2;
        const ssize_t ret =
            writev(fd, iov.data(), static_cast<int>(iov_count));
        if (ret < 0)
        {
            cfg.logger_->error(
                "baseband: writev failed: {}",
                strerror(errno));
            return;
        }
        current_size += ret;
    }
};

baseband::baseband(int stream_id): m_stream_id(stream_id)
{
    auto &cfg = GlobalConfig::getInstance();
    m_queue = &cfg.streams[m_stream_id].queue;
    m_subband_id = m_stream_id/2;

    // m_queueB = &cfg.streams[m_subband_id * 2 + 1].queue;
    if (m_subband_id < 4)
        m_folder = cfg.Baseband_folder0;
    else
        m_folder = cfg.Baseband_folder1;
    if (create_file() != 0)
    {
        // best-effort logging; constructor cannot throw per your preference
        cfg.logger_->error("baseband: initial create_file failed for subband {}", m_subband_id);
    }
}

baseband::~baseband()
{
    if (fd >= 0)close(fd);
    // if (fd_y >= 0)close(fd_y);
}
