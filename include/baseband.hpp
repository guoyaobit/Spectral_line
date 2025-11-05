#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <Globalcfg.hpp>

class baseband
{
private:
    /* data */
    int m_subband_id;
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueA; // 队列
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueB;
    int sockfdA,sockfdB;

public:
    baseband(int);
    ~baseband();
    void collectdata()
    {
        PacketBatch *readblockA = nullptr;
        PacketBatch *readblockB = nullptr;
        m_queueA->wait_dequeue(readblockA);
        m_queueB->wait_dequeue(readblockB);

    

        for (int i =0;i<readblockA->pkts.size();i++)
        {
            // readblockA->pkts[i]->payload
        }
    }
    
};

baseband::baseband(int sub_band_id)
{
    auto &cfg = GlobalConfig::getInstance();

    m_queueA = &cfg.g_in_queues[m_subband_id * 2];

    m_queueB = &cfg.g_in_queues[m_subband_id * 2 + 1];
    socket(AF_INET, SOCK_STREAM, 0);
    // 创建 UDP socket
    sockfdA = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfdA < 0) {
        perror("socket error");
        exit(1);
    }
    sockfdB = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfdB < 0) {
        perror("socket error");
        exit(1);
    }

    // memset(&servaddr, 0, sizeof(servaddr));
    // servaddr.sin_family = AF_INET;
    // servaddr.sin_port = htons(8888);
    // inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr); // 发送到本地

}

baseband::~baseband()
{
}
