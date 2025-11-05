#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#pragma pack(push, 1)
typedef struct
{
    uint64_t timestamp_ns;
    uint16_t subband_id; // 子频段编号
    uint16_t window_id;  // window id
    uint32_t subband_start_freq;
    uint32_t subband_end_freq;
    float start_freq_hz;
    float channel_bw_hz;
    uint16_t pkt_id;
    uint16_t total_pkt;
    uint32_t n_channels;
    char cal_mode[16]; // Cal mode (OFF, SYNC, EXT1, EXT2)
    double cal_freq;   // Cal modulation frequency (Hz)
    double cal_dcyc;   // Cal duty cycle (0-1)
    double cal_phs;    // Cal phase (wrt start time)
    double ra;         // RA mid-integration
    double dec;        // DEC mid-integration
} spectrum_header;
#pragma pack(pop)

class SpectrumSender
{
public:
    SpectrumSender() : sockfd_(-1)
    {
        // 不初始化 socket，用户需要后续调用 init() 才能用
    }

    SpectrumSender(const std::string &ip, uint16_t port)
    {
        init(ip, port);
    }

    void init(const std::string &ip, uint16_t port)
    {
        sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd_ < 0)
        {
            perror("socket creation failed");
            throw std::runtime_error("Failed to create socket");
        }
        memset(&dest_addr_, 0, sizeof(dest_addr_));
        dest_addr_.sin_family = AF_INET;
        dest_addr_.sin_port = htons(port);

        if (inet_pton(AF_INET, ip.c_str(), &dest_addr_.sin_addr) <= 0)
        {
            close(sockfd_);
            throw std::runtime_error("Invalid IP address");
        }
    }

    ~SpectrumSender()
    {
        if (sockfd_ >= 0)
        {
            close(sockfd_);
            // printf("socket closed\n");
        }
    }

    // bool send(spectrum_header &header, const void *payload, size_t payload_len)
    // {
    //     if (sockfd_ < 0)
    //     {
    //         std::cerr << "❌ Socket not initialized!" << std::endl;
    //         return false;
    //     }

    //     const char *payload_bytes = reinterpret_cast<const char *>(payload);
    //     size_t offset = 0;
    //     const size_t header_size = sizeof(spectrum_header);
    //     const int mtu = 8192+header_size;
    //     header.total_pkt = payload_len /8192;
    //     if(header.total_pkt*8192 < payload_len)
    //     {
    //         header.total_pkt+=1;
    //     }
    //     for ( header.pkt_id =0;header.pkt_id < header.total_pkt;header.pkt_id++)
    //     {
    //         // printf("%d\n",offset);
    //         size_t chunk_size = std::min(static_cast<size_t>(mtu), payload_len - offset);
    //         size_t total_len = header_size + chunk_size;

    //         std::vector<char> buffer(total_len);

    //         // 拷贝头部
    //         memcpy(buffer.data(), &header, header_size);
    //         // 拷贝当前分片
    //         memcpy(buffer.data() + header_size, payload_bytes + offset, chunk_size);

    //         ssize_t sent = sendto(sockfd_, buffer.data(), total_len, 0,
    //                               reinterpret_cast<sockaddr *>(&dest_addr_),
    //                               sizeof(dest_addr_));
    //         if (sent < 0)
    //         {
    //             perror("sendto failed");
    //             return false;
    //         }
    //         offset += chunk_size;
    //     }
    // }
    bool send_spectrum(spectrum_header &header, const void *payload, size_t payload_len)
    {
        if (sockfd_ < 0)
        {
            std::cerr << "❌ Socket not initialized!" << std::endl;
            return false;
        }
        // printf("%f\n",header.start_freq_hz);
        const char *payload_bytes = reinterpret_cast<const char *>(payload);
        const size_t header_size = sizeof(spectrum_header);
        const size_t chunk_data_size = 8192;

        header.total_pkt = static_cast<uint16_t>((payload_len + chunk_data_size - 1) / chunk_data_size);

        std::vector<char> buffer(header_size + chunk_data_size);
        size_t offset = 0;

        for (header.pkt_id = 0; header.pkt_id < header.total_pkt; header.pkt_id++)
        {
            size_t chunk_size = std::min(chunk_data_size, payload_len - offset);

            memcpy(buffer.data(), &header, header_size);
            memcpy(buffer.data() + header_size, payload_bytes + offset, chunk_size);

            ssize_t sent = sendto(sockfd_, buffer.data(), header_size + chunk_size, 0,
                                  reinterpret_cast<const sockaddr *>(&dest_addr_),
                                  sizeof(dest_addr_));
            if (sent < 0)
            {
                perror("sendto failed");
                return false;
            }

            offset += chunk_size;
        }

        return true;
    }

private:
    int sockfd_;
    sockaddr_in dest_addr_;
};
