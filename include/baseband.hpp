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
    int m_subband_id;
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueA;
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueB;
    int fd = -1;
    static constexpr uint64_t DEFAULT_MAX_FILE_SIZE = 4ULL * 1024 * 1024 * 1024; // 4 GB
    uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE;
    uint64_t current_size = 0;
    uint32_t file_index = 0;
    std::string m_folder;

    static constexpr size_t FRAME_PAYLOAD = 8192;
    uint8_t vdif_payloadA[FRAME_PAYLOAD];
    uint8_t vdif_payloadB[FRAME_PAYLOAD];
    static constexpr size_t HEADER_SIZE = 32;

    // create_file: returns 0 on success, -1 on error
    int create_file()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
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

        char basename[64];
        // snprintf used per preference
        if (std::snprintf(basename, sizeof(basename), "subband_%02d_%04u_dual.vdif", m_subband_id, file_index++) < 0)
        {
            GlobalConfig::getInstance().logger_->error("baseband: snprintf failed for filename");
            return -1;
        }

        std::filesystem::path filepath = folder / basename;
        fd = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
        {
            auto &cfg = GlobalConfig::getInstance();
            cfg.logger_->error("baseband: open('{}') failed: {} (errno={})", filepath.string(), std::strerror(errno), errno);
            return -1;
        }
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
    // recoder handles errors locally (logs + returns) rather than throwing
    void recoder()
    {
        auto &cfg = GlobalConfig::getInstance();
        PacketBatch *readblockA = nullptr;
        PacketBatch *readblockB = nullptr;
        m_queueA->wait_dequeue(readblockA);
        m_queueB->wait_dequeue(readblockB);

        if (m_queueA->size_approx() > cfg.QUEUE_CAPACITY * 0.95)
            cfg.logger_->debug("BaseBand: Buffered {} batch in queue, >95%%", m_queueA->size_approx());

        // determine how many frames to write: use actual count (A and B should be equal; use minCount)
        int frames_to_write = readblockA->count;

        // TODO
        if (cfg.Baseband_bits == 4)
        {
            if (current_size > max_file_size)
            {
                if (create_file() != 0)
                {
                    cfg.logger_->error("baseband: create_file failed during rotation");
                    return;
                }
            }
            constexpr int MAX_FRAMES_PER_WRITEV = 256;

            iovec iov[MAX_FRAMES_PER_WRITEV * 4];

            for (int base = 0; base < frames_to_write;)
            {
                int batch = std::min(
                    MAX_FRAMES_PER_WRITEV,
                    frames_to_write - base);

                int idx = 0;

                for (int i = 0; i < batch; i++)
                {
                    int frame = base + i;

                    quantize4_avx512(
                        readblockA->pkts[frame]->payload,
                        FRAME_PAYLOAD);

                    quantize4_avx512(
                        readblockB->pkts[frame]->payload,
                        FRAME_PAYLOAD);
                    iov[idx++] = {
                        &readblockA->hdrs[frame],
                        HEADER_SIZE
                    };
                    iov[idx++] = {
                        readblockA->pkts[frame]->payload,
                        FRAME_PAYLOAD / 2
                    };
                    iov[idx++] = {
                        &readblockB->hdrs[frame],
                        HEADER_SIZE
                    };
                    iov[idx++] = {
                        readblockB->pkts[frame]->payload,
                        FRAME_PAYLOAD / 2
                    };
                }
                ssize_t ret = writev(fd, iov, idx);
                size_t expected =
                    batch *
                    (2 * HEADER_SIZE + FRAME_PAYLOAD);

                if (ret != expected)
                {
                    cfg.logger_->error(
                        "writev incomplete {} / {}",
                        ret,
                        expected);
                    return;
                }


                base += batch;
            }
            return;
        }
        uint64_t bytes_to_write = static_cast<uint64_t>(frames_to_write) * (FRAME_PAYLOAD + HEADER_SIZE) * 2;
        if (current_size + bytes_to_write > max_file_size)
        {
            if (create_file() != 0)
            {
                cfg.logger_->error("baseband: create_file failed during rotation");
                return;
            }
        }
        iovec iov[4];
        for (int i = 0; i < frames_to_write; ++i)
        {
            iov[0].iov_base = &readblockA->hdrs[i];
            iov[0].iov_len = HEADER_SIZE;

            iov[1].iov_base = readblockA->pkts[i]->payload;
            iov[1].iov_len = FRAME_PAYLOAD;

            iov[2].iov_base = &readblockB->hdrs[i];
            iov[2].iov_len = HEADER_SIZE;

            iov[3].iov_base = readblockB->pkts[i]->payload;
            iov[3].iov_len = FRAME_PAYLOAD;

            writev(fd, iov, 4);
        }
        current_size += bytes_to_write;
    }
};

baseband::baseband(int sub_band_id)
    : m_subband_id(sub_band_id)
{
    auto &cfg = GlobalConfig::getInstance();
    m_queueA = &cfg.streams[m_subband_id * 2].queue;
    m_queueB = &cfg.streams[m_subband_id * 2 + 1].queue;
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
    if (fd >= 0)
        ::close(fd);
}
