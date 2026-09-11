#pragma once
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#pragma pack(push, 1)
typedef struct {
  uint32_t magic = 0x534C5231; // "SLR1"
  uint16_t version = 1;
  // UTC integration center time
  // Unix epoch nanoseconds
  uint64_t timestamp_ns;
  // observation identification
  uint32_t obs_id;         // observation ID
  uint32_t integration_id; // integration counter
  // frequency information
  uint16_t subband_id; // Subband identifier
  uint16_t window_id;  // window id
  double subband_start_freq;
  double subband_end_freq;
  double start_freq_hz;
  double channel_bw_hz;
  uint32_t n_channels;
  uint8_t stokes;
  // packet fragmentation
  uint16_t pkt_id;
  uint16_t total_pkt;
  // integration
  float exposure; // seconds
  // calibration
  uint8_t noise_state; // OFF=0 ON=1 MIX=2
  uint8_t cal_mode;    // optional
  uint16_t reserved2;

  // telescope direction
  double ra;  // rad
  double dec; // rad
  // data quality
  uint32_t flags; // overflow/dropout/etc
} spectrum_header;
#pragma pack(pop)

class SpectrumSender {
public:
  SpectrumSender() : sockfd_(-1) {}

  SpectrumSender(const std::string &ip, uint16_t port) { init(ip, port); }

  void init(const std::string &ip, uint16_t port) {
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
      perror("socket creation failed");
      throw std::runtime_error("Failed to create socket");
    }
    memset(&dest_addr_, 0, sizeof(dest_addr_));
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &dest_addr_.sin_addr) <= 0) {
      close(sockfd_);
      throw std::runtime_error("Invalid IP address");
    }
  }

  ~SpectrumSender() {
    if (sockfd_ >= 0) {
      close(sockfd_);
    }
  }
  bool send_spectrum(spectrum_header &header, const void *payload,
                     size_t payload_len) {
    if (sockfd_ < 0) {
      std::cerr << "Socket is not initialized" << std::endl;
      return false;
    }
    const char *payload_bytes = reinterpret_cast<const char *>(payload);
    const size_t header_size = sizeof(spectrum_header);
    const size_t chunk_data_size = 8192;

    header.total_pkt = static_cast<uint16_t>(
        (payload_len + chunk_data_size - 1) / chunk_data_size);

    std::vector<char> buffer(header_size + chunk_data_size);
    size_t offset = 0;

    for (header.pkt_id = 0; header.pkt_id < header.total_pkt; header.pkt_id++) {
      size_t chunk_size = std::min(chunk_data_size, payload_len - offset);

      memcpy(buffer.data(), &header, header_size);
      memcpy(buffer.data() + header_size, payload_bytes + offset, chunk_size);

      ssize_t sent = sendto(sockfd_, buffer.data(), header_size + chunk_size, 0,
                            reinterpret_cast<const sockaddr *>(&dest_addr_),
                            sizeof(dest_addr_));
      if (sent < 0) {
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
