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

#include <Globalcfg.hpp>
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

    // write_all: returns 0 on success, -1 on error (logs)
    static int write_all(int fd, const void *buf, size_t count)
    {
        const char *p = static_cast<const char *>(buf);
        size_t remaining = count;
        while (remaining > 0)
        {
            ssize_t nw = ::write(fd, p, remaining);
            if (nw < 0)
            {
                if (errno == EINTR)
                    continue;
                auto &cfg = GlobalConfig::getInstance();
                cfg.logger_->error("baseband: write failed: {} (errno={})", std::strerror(errno), errno);
                return -1;
            }
            remaining -= static_cast<size_t>(nw);
            p += nw;
        }
        return 0;
    }

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
    float calc_rms(PacketBatch *batch)
    {
        double power = 0.0;
        uint64_t count = 0;

        for (int p = 0; p < batch->count; p++)
        {
            const int8_t *src =
                reinterpret_cast<const int8_t *>(
                    batch->pkts[p]->payload);

            // IQ IQ IQ
            for (size_t i = 0; i < FRAME_PAYLOAD; i += 2)
            {
                int8_t I = src[i];
                int8_t Q = src[i + 1];

                power +=
                    double(I) * I +
                    double(Q) * Q;

                count += 2;
            }
        }

        return sqrt(power / count);
    }

    // collectdata handles errors locally (logs + returns) rather than throwing
    void collectdata()
    {
        auto &cfg = GlobalConfig::getInstance();
        PacketBatch *readblockA = nullptr;
        PacketBatch *readblockB = nullptr;
        m_queueA->wait_dequeue(readblockA);
        m_queueB->wait_dequeue(readblockB);

        if (!readblockA || !readblockB)
        {
            cfg.logger_->error("baseband: dequeued null PacketBatch (A={}, B={})", (void *)readblockA, (void *)readblockB);
            return;
        }

        if (m_queueA->size_approx() > cfg.QUEUE_CAPACITY * 0.95)
            cfg.logger_->debug("BaseBand: Buffered {} batch in queue, >95%%", m_queueA->size_approx());

        // use actual count (important fix)
        int countA = readblockA->count;
        int countB = readblockB->count;
        bool invalidDetected = false;

        if (countA != countB)
        {
            cfg.logger_->error("Packet count mismatch: A.count={} B.count={}", countA, countB);
            invalidDetected = true;
        }

        size_t m_pktidA = (countA > 0) ? readblockA->pkt_id[0] : 0;
        size_t m_pktidB = (countB > 0) ? readblockB->pkt_id[0] : 0;

        int minCount = std::min(countA, countB);
        for (int i = 0; i < minCount; ++i)
        {
            if (readblockA->pkt_id[i] != m_pktidA + static_cast<size_t>(i))
                invalidDetected = true;
            if (readblockB->pkt_id[i] != m_pktidB + static_cast<size_t>(i))
                invalidDetected = true;
        }

        if (invalidDetected)
        {
            cfg.logger_->warn("baseband: invalid packet(s) detected in subband {}, zeroing {} entries", m_subband_id, minCount);
            std::memset(readblockA->buffer, 0, static_cast<size_t>(minCount) * sizeof(Packet));
            std::memset(readblockB->buffer, 0, static_cast<size_t>(minCount) * sizeof(Packet));
            // choose semantic: mark as repaired so it's written as silence
            readblockA->valid = true;
            readblockB->valid = true;
        }

        // determine how many frames to write: use actual count (A and B should be equal; use minCount)
        int frames_to_write = minCount;
        if (frames_to_write <= 0)
        {
            cfg.logger_->debug("baseband: zero frames to write for subband {}", m_subband_id);
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
        float rmsA = calc_rms(readblockA);
        float rmsB = calc_rms(readblockB);
        float gain4A =
            7.0f / (3.0f * rmsA);

        float gain4B =
            7.0f / (3.0f * rmsB);

        if (fd < 0)
        {
            cfg.logger_->error("baseband: invalid file descriptor before write");
            return;
        }
        uint32_t frame_no = 0;
        size_t payload_size_A;
        size_t payload_size_B;
        for (int i = 0; i < frames_to_write; ++i)
        {
            if (cfg.Baseband_bits == 8)
            {
                payload_size_A = FRAME_PAYLOAD;
                payload_size_B = FRAME_PAYLOAD;
                memcpy(vdif_payloadA,
                       readblockA->pkts[i]->payload,
                       FRAME_PAYLOAD);
                memcpy(vdif_payloadB,
                       readblockB->pkts[i]->payload,
                       FRAME_PAYLOAD);
            }
            else if (cfg.Baseband_bits == 4)
            {
                payload_size_A =
                    pack_4bit(
                        readblockA->pkts[i]->payload,
                        vdif_payloadA,
                        gain4A);

                payload_size_B =
                    pack_4bit(
                        readblockB->pkts[i]->payload,
                        vdif_payloadB,
                        gain4B);
            }
            else
            {
                payload_size_A =
                    pack_2bit(
                        readblockA->pkts[i]->payload,
                        vdif_payloadA,
                        rmsA);

                payload_size_B =
                    pack_2bit(
                        readblockB->pkts[i]->payload,
                        vdif_payloadB,
                        rmsB);
            }
            readblockA->hdrs[i].setThread(m_subband_id * 2);
            readblockA->hdrs[i].setFrameLength(
                HEADER_SIZE + payload_size_A);
            if (write_all(fd, readblockA->hdrs[i].data(), HEADER_SIZE) != 0)
                return;
            if (write_all(fd, vdif_payloadA, payload_size_A) != 0)
                return;
            readblockB->hdrs[i].setThread(m_subband_id * 2 + 1);
            readblockB->hdrs[i].setFrameLength(
                HEADER_SIZE + payload_size_A);
            if (write_all(fd, readblockB->hdrs[i].data(), HEADER_SIZE) != 0)
                return;
            if (write_all(fd, vdif_payloadB, payload_size_B) != 0)
                return;
            current_size +=
                HEADER_SIZE + payload_size_A +
                HEADER_SIZE + payload_size_B;
        }
    }
    size_t pack_4bit(
        const uint8_t *src,
        uint8_t *dst,
        float gain)
    {
        const int8_t *input =
            reinterpret_cast<const int8_t *>(src);

        size_t out = 0;

        for (size_t i = 0; i < FRAME_PAYLOAD; i += 2)
        {
            int a = lrintf(input[i] * gain);
            int b = lrintf(input[i + 1] * gain);

            a = std::clamp(a, -8, 7);
            b = std::clamp(b, -8, 7);

            dst[out++] =
                ((a & 0xf) << 4) |
                (b & 0xf);
        }

        return out;
    }
    inline uint8_t q2(float x)
    {
        if (x < -0.981)
            return 0;

        if (x < 0)
            return 1;

        if (x < 0.981)
            return 2;

        return 3;
    }
    size_t pack_2bit(
        const uint8_t *src,
        uint8_t *dst,
        float rms)
    {
        const int8_t *input =
            reinterpret_cast<const int8_t *>(src);

        size_t out = 0;

        uint8_t temp = 0;
        int shift = 6;

        for (size_t i = 0; i < FRAME_PAYLOAD; i++)
        {
            uint8_t q =
                q2(input[i] / rms);

            temp |= q << shift;

            if (shift == 0)
            {
                dst[out++] = temp;
                temp = 0;
                shift = 6;
            }
            else
            {
                shift -= 2;
            }
        }

        return out;
    }
};

baseband::baseband(int sub_band_id)
    : m_subband_id(sub_band_id)
{
    auto &cfg = GlobalConfig::getInstance();
    // m_queueA = &cfg.stream_queues[m_subband_id * 2];
    // m_queueB = &cfg.stream_queues[m_subband_id * 2 + 1];
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