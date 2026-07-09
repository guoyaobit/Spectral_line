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
#include <VDIFheader.hpp>
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
        if (ec)
        {
            auto &cfg = GlobalConfig::getInstance();
            cfg.logger_->error("baseband: create_directories({}) failed: {}", folder.string(), ec.message());
            return -1;
        }

        char basename[64];
        // snprintf used per preference
        if (std::snprintf(basename, sizeof(basename), "subband_%02d_%04u.vdif", m_subband_id, file_index++) < 0)
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

        VDIFHeader header;
        header.setFrameLength(HEADER_SIZE + FRAME_PAYLOAD, 8);
        header.setStation(0x5154);

        if (fd < 0)
        {
            cfg.logger_->error("baseband: invalid file descriptor before write");
            return;
        }

        for (int i = 0; i < frames_to_write; ++i)
        {
            header.setThread(m_subband_id * 2);
            if (write_all(fd, header.data(), HEADER_SIZE) != 0)
                return;
            if (write_all(fd, readblockA->pkts[i], FRAME_PAYLOAD) != 0)
                return;

            header.setThread(m_subband_id * 2 + 1);
            if (write_all(fd, header.data(), HEADER_SIZE) != 0)
                return;
            if (write_all(fd, readblockB->pkts[i], FRAME_PAYLOAD) != 0)
                return;
        }

        current_size += bytes_to_write;

        // TODO: return/release PacketBatch to pool per project semantics
    }
};

baseband::baseband(int sub_band_id)
    : m_subband_id(sub_band_id)
{
    auto &cfg = GlobalConfig::getInstance();
    m_queueA = &cfg.g_in_queues[m_subband_id * 2];
    m_queueB = &cfg.g_in_queues[m_subband_id * 2 + 1];

    if (m_subband_id < 4)
        m_folder = cfg.Baseband_folder0;
    else
        m_folder = cfg.Baseband_folder1;

    if (create_file() != 0)
    {
        // best-effort logging; constructor cannot throw per your preference
        cfg.logger_->error("baseband: initial create_file failed for subband {}", m_subband_id);
        // decide: keep fd=-1 and let collectdata handle later, or abort process:
        // cfg.logger_->critical("baseband: aborting due to initial file creation failure"); std::abort();
    }
}

baseband::~baseband()
{
    if (fd >= 0)
        ::close(fd);
}