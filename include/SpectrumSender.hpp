#pragma once

#include <SpectrumTransport.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <zmq.h>

#pragma pack(push, 1)
typedef struct {
  uint32_t magic = 0x534C5231; // "SLR1"
  uint16_t version = 2;
  uint64_t timestamp_ns; // UTC integration centre, Unix epoch nanoseconds
  uint32_t obs_id;
  uint32_t integration_id;
  uint16_t subband_id;
  uint16_t window_id;
  double subband_start_freq;
  double subband_end_freq;
  double start_freq_hz;
  double channel_bw_hz;
  uint32_t n_channels;
  uint8_t stokes;
  // A ZeroMQ message contains one complete result. These fields remain for
  // spectrum_header v2 layout compatibility and are always 0/1.
  uint16_t pkt_id;
  uint16_t total_pkt;
  float exposure;
  uint8_t noise_state;
  uint8_t cal_mode;
  uint8_t beam_id = 0;
  uint8_t reserved2 = 0;
  double ra;
  double dec;
  uint32_t flags;
} spectrum_header;
#pragma pack(pop)

static_assert(sizeof(spectrum_header) == 95,
              "spectrum_header protocol size must remain 95 bytes");
static_assert(offsetof(spectrum_header, beam_id) == 73,
              "spectrum_header beam_id offset must remain stable");

inline void *spectrum_zmq_context() {
  // Process-lifetime storage avoids static destruction ordering problems with
  // SpectrumSender objects owned by GlobalConfig.
  static void *context = [] {
    void *ctx = zmq_ctx_new();
    if (ctx != nullptr && zmq_ctx_set(ctx, ZMQ_IO_THREADS, 2) != 0) {
      zmq_ctx_term(ctx);
      ctx = nullptr;
    }
    return ctx;
  }();
  return context;
}

class SpectrumSender {
public:
  SpectrumSender() = default;
  SpectrumSender(const SpectrumSender &) = delete;
  SpectrumSender &operator=(const SpectrumSender &) = delete;

  ~SpectrumSender() {
    if (socket_ != nullptr)
      zmq_close(socket_);
  }

  void init(const std::string &ip, uint16_t port, uint16_t server_id) {
    if (socket_ != nullptr)
      zmq_close(socket_);

    void *context = spectrum_zmq_context();
    if (context == nullptr)
      throw std::runtime_error("Failed to create ZeroMQ context");

    socket_ = zmq_socket(context, ZMQ_PUSH);
    if (socket_ == nullptr)
      throw std::runtime_error(zmq_strerror(zmq_errno()));

    server_id_ = server_id;
    const int one = 1;
    const int zero = 0;
    // Keep at most one result per logical window in ZeroMQ. This bounds stale
    // data when the Writer stops and makes subsequent non-blocking sends drop.
    const int send_hwm = 1;
    const int heartbeat_ms = 1000;
    const int heartbeat_timeout_ms = 3000;
    if (zmq_setsockopt(socket_, ZMQ_IMMEDIATE, &one, sizeof(one)) != 0 ||
        zmq_setsockopt(socket_, ZMQ_LINGER, &zero, sizeof(zero)) != 0 ||
        zmq_setsockopt(socket_, ZMQ_SNDHWM, &send_hwm,
                       sizeof(send_hwm)) != 0 ||
        zmq_setsockopt(socket_, ZMQ_SNDTIMEO, &zero, sizeof(zero)) != 0 ||
        zmq_setsockopt(socket_, ZMQ_HEARTBEAT_IVL, &heartbeat_ms,
                       sizeof(heartbeat_ms)) != 0 ||
        zmq_setsockopt(socket_, ZMQ_HEARTBEAT_TIMEOUT,
                       &heartbeat_timeout_ms,
                       sizeof(heartbeat_timeout_ms)) != 0) {
      const std::string error = zmq_strerror(zmq_errno());
      zmq_close(socket_);
      socket_ = nullptr;
      throw std::runtime_error("Failed to configure ZeroMQ sender: " + error);
    }

    endpoint_ = "tcp://" + ip + ":" + std::to_string(port);
    if (zmq_connect(socket_, endpoint_.c_str()) != 0) {
      const std::string error = zmq_strerror(zmq_errno());
      zmq_close(socket_);
      socket_ = nullptr;
      throw std::runtime_error("Failed to connect ZeroMQ sender to " +
                               endpoint_ + ": " + error);
    }
  }

  bool send_spectrum(spectrum_header &header, const void *payload,
                     size_t payload_len) {
    if (socket_ == nullptr || payload == nullptr)
      return false;
    if (payload_len >
        std::numeric_limits<uint32_t>::max() - sizeof(spectrum_header))
      return false;

    header.pkt_id = 0;
    header.total_pkt = 1;

    // Advance before checking writability so a mid-run disconnect remains
    // visible as a sequence gap after the Writer reconnects. Avoid allocating,
    // copying, and checksumming a large result when no peer can accept it.
    const uint64_t sequence = next_sequence_++;
    zmq_pollitem_t writable{socket_, 0, ZMQ_POLLOUT, 0};
    const int poll_result = zmq_poll(&writable, 1, 0);
    if (poll_result <= 0 || (writable.revents & ZMQ_POLLOUT) == 0)
      return false;

    const size_t body_bytes = sizeof(spectrum_header) + payload_len;
    const size_t message_bytes = sizeof(spectrum_transport_header) + body_bytes;
    if (message_bytes > static_cast<size_t>(std::numeric_limits<int>::max()))
      return false;

    // Allocate the final libzmq message directly. zmq_msg_send() transfers its
    // storage to the I/O thread, avoiding the extra copy performed by
    // zmq_send() from an application-owned std::vector.
    zmq_msg_t message;
    if (zmq_msg_init_size(&message, message_bytes) != 0)
      return false;
    auto *message_data = static_cast<uint8_t *>(zmq_msg_data(&message));

    auto *transport = reinterpret_cast<spectrum_transport_header *>(
        message_data);
    *transport = spectrum_transport_header{};
    transport->server_id = server_id_;
    transport->sequence = sequence;
    transport->message_bytes = static_cast<uint32_t>(body_bytes);

    uint8_t *body = message_data + sizeof(*transport);
    std::memcpy(body, &header, sizeof(header));
    std::memcpy(body + sizeof(header), payload, payload_len);
    transport->crc32c = spectrum_crc32c(body, body_bytes);

    const int sent = zmq_msg_send(&message, socket_, ZMQ_DONTWAIT);
    const bool complete =
        sent >= 0 && static_cast<size_t>(sent) == message_bytes;
    zmq_msg_close(&message);
    return complete;
  }

private:
  void *socket_ = nullptr;
  uint16_t server_id_ = 0;
  uint64_t next_sequence_ = 0;
  std::string endpoint_;
};
