#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <Globalcfg.hpp>
#include <VDIFheader.hpp>
class baseband
{
private:
    /* data */
    int m_subband_id;
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueA; // 队列
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueB;
    // int sockfdA, sockfdB;
    int fd;
    const uint64_t MAX_FILE_SIZE = 4ULL * 1024 * 1024 * 1024; // 4 GB
    uint64_t current_size = 0;
    uint32_t file_index = 0;
    std::string m_folder;

public:
    baseband(int);
    ~baseband();
    void collectdata()
    {
        auto &cfg = GlobalConfig::getInstance();
        PacketBatch *readblockA = nullptr;
        PacketBatch *readblockB = nullptr;
        m_queueA->wait_dequeue(readblockA);
        m_queueB->wait_dequeue(readblockB);
        if (m_queueA->size_approx() > cfg.QUEUE_CAPACITY * 0.95)
            cfg.logger_->debug("BseBand: Buffed {} batch in queue,more than 95%%", m_queueA->size_approx());
        size_t m_pktidA = readblockA->pkt_id[0];
        size_t m_pktidB = readblockB->pkt_id[0];
        // error processing
        for (int i = 0; i < cfg.batchsize(); i++)
        {
            if (readblockA->pkt_id[i] != m_pktidA + i)
                readblockA->valid = false;
            if (readblockB->pkt_id[i] != m_pktidB + i)
                readblockB->valid = false;
        }

        if (readblockA->valid == false or readblockB->valid == false)
        {
            memset(readblockA->buffer, 0, cfg.batchsize() * sizeof(Packet));
            memset(readblockB->buffer, 0, cfg.batchsize() * sizeof(Packet));
        }
        // 2,4,8bit compress
        // compressdata()；
        if (current_size >= MAX_FILE_SIZE)
        {
            close(fd);

            ++file_index;

            char filename[256];

            snprintf(filename,
                     sizeof(filename),
                     "%s/subband_%02d_%04d.vdif",
                     m_folder.c_str(),
                     m_subband_id,
                     file_index);
            fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }

        for (int i = 0; i < readblockA->count; i++)
        {
            VDIFHeader hA;
            hA.setFrameLength(
                32 + 8192,
                8);

            hA.setThread(m_subband_id);

            hA.setStation(0x5154); // QT

            write(fd,
                  hA.data(),
                  32);
            write(fd, readblockA->pkts[i], 8192);
            write(fd, readblockA->pkts[i], 8192);
        }
        current_size += readblockA->count * 8192 * 2;
    };

    baseband::baseband(int sub_band_id)
    {
        auto &cfg = GlobalConfig::getInstance();
        m_queueA = &cfg.g_in_queues[m_subband_id * 2];
        m_queueB = &cfg.g_in_queues[m_subband_id * 2 + 1];
        // pre 4 subband in first disk and after 4 subband in second disk
        if (m_subband_id / 4 == 0)
            m_folder = cfg.Baseband_folder0;
        else
            m_folder = cfg.Baseband_folder1;
        char filename[256];
        snprintf(filename,
                 sizeof(filename),
                 "%s/subband_%02d_%04d.vdif",
                 m_folder.c_str(),
                 m_subband_id,
                 file_index);
        fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    baseband::~baseband()
    {
    }
};