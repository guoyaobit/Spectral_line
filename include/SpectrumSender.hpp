#pragma once

#include <SpectrumTransport.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
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
  enum class ConnectionEvent { CONNECTED, DISCONNECTED };

  SpectrumSender() = default;
  SpectrumSender(const SpectrumSender &) = delete;
  SpectrumSender &operator=(const SpectrumSender &) = delete;

  ~SpectrumSender() {
    // A ZeroMQ socket must be closed by its owning thread. During normal
    // operation the GPU worker lives for the process lifetime, so static
    // teardown may run on another thread; the OS then releases the socket.
    if (socket_ != nullptr && owner_thread_ == std::this_thread::get_id())
      zmq_close(socket_);
    if (monitor_socket_ != nullptr &&
        owner_thread_ == std::this_thread::get_id())
      zmq_close(monitor_socket_);
  }

  void init(const std::string &ip, uint16_t port, uint16_t server_id) {
    // init() runs while config.yaml is parsed on the main thread. Do not
    // create the socket here: send_spectrum() runs on a GPU worker, and
    // ZeroMQ sockets must never be used from a thread other than their owner.
    server_id_ = server_id;
    endpoint_ = "tcp://" + ip + ":" + std::to_string(port);
  }

  const std::string &endpoint() const { return endpoint_; }
  const std::string &last_failure() const { return last_failure_; }
  bool prepare() { return ensure_socket(); }
  bool wait_until_writable(long timeout_ms) {
    if (!ensure_socket())
      return false;
    zmq_pollitem_t writable{socket_, 0, ZMQ_POLLOUT, 0};
    const int poll_result = zmq_poll(&writable, 1, timeout_ms);
    if (poll_result < 0) {
      last_failure_ = zmq_strerror(zmq_errno());
      return false;
    }
    if (poll_result == 0 || (writable.revents & ZMQ_POLLOUT) == 0) {
      last_failure_ = "no writable peer (connection pending/down or HWM full)";
      return false;
    }
    last_failure_.clear();
    return true;
  }

  std::vector<ConnectionEvent> take_connection_events() {
    std::vector<ConnectionEvent> events;
    if (monitor_socket_ == nullptr)
      return events;

    while (true) {
      zmq_msg_t event_frame;
      if (zmq_msg_init(&event_frame) != 0)
        break;
      const int received =
          zmq_msg_recv(&event_frame, monitor_socket_, ZMQ_DONTWAIT);
      if (received < 0) {
        zmq_msg_close(&event_frame);
        break;
      }

      uint16_t event_id = 0;
      if (zmq_msg_size(&event_frame) >= sizeof(event_id))
        std::memcpy(&event_id, zmq_msg_data(&event_frame), sizeof(event_id));
      const bool has_address = zmq_msg_more(&event_frame) != 0;
      zmq_msg_close(&event_frame);

      if (has_address) {
        zmq_msg_t address_frame;
        if (zmq_msg_init(&address_frame) == 0) {
          zmq_msg_recv(&address_frame, monitor_socket_, ZMQ_DONTWAIT);
          zmq_msg_close(&address_frame);
        }
      }

      if (event_id == ZMQ_EVENT_CONNECTED && !monitor_connected_) {
        monitor_connected_ = true;
        events.push_back(ConnectionEvent::CONNECTED);
      } else if (event_id == ZMQ_EVENT_DISCONNECTED && monitor_connected_) {
        monitor_connected_ = false;
        events.push_back(ConnectionEvent::DISCONNECTED);
      }
    }
    return events;
  }

  bool send_spectrum(spectrum_header &header, const void *payload,
                     size_t payload_len) {
    if (!ensure_socket())
      return false;
    if (payload == nullptr) {
      last_failure_ = "result payload is null";
      return false;
    }
    if (payload_len >
        std::numeric_limits<uint32_t>::max() - sizeof(spectrum_header)) {
      last_failure_ = "payload exceeds transport size limit";
      return false;
    }

    header.pkt_id = 0;
    header.total_pkt = 1;

    // Advance before checking writability so a mid-run disconnect remains
    // visible as a sequence gap after the Writer reconnects. Avoid allocating,
    // copying, and checksumming a large result when no peer can accept it.
    const uint64_t sequence = next_sequence_++;
    if (!wait_until_writable(0))
      return false;

    const size_t body_bytes = sizeof(spectrum_header) + payload_len;
    const size_t message_bytes = sizeof(spectrum_transport_header) + body_bytes;
    if (message_bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
      last_failure_ = "message exceeds ZeroMQ send size limit";
      return false;
    }

    // Allocate the final libzmq message directly. zmq_msg_send() transfers its
    // storage to the I/O thread, avoiding the extra copy performed by
    // zmq_send() from an application-owned std::vector.
    zmq_msg_t message;
    if (zmq_msg_init_size(&message, message_bytes) != 0) {
      last_failure_ = zmq_strerror(zmq_errno());
      return false;
    }
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
    if (!complete)
      last_failure_ = sent < 0 ? zmq_strerror(zmq_errno())
                               : "partial ZeroMQ message send";
    else
      last_failure_.clear();
    zmq_msg_close(&message);
    return complete;
  }

private:
  bool ensure_socket() {
    if (socket_ != nullptr) {
      if (owner_thread_ != std::this_thread::get_id()) {
        last_failure_ = "ZeroMQ socket accessed from a non-owner thread";
        return false;
      }
      return true;
    }
    if (endpoint_.empty()) {
      last_failure_ = "ZeroMQ sender endpoint is not configured";
      return false;
    }

    void *context = spectrum_zmq_context();
    if (context == nullptr) {
      last_failure_ = "failed to create ZeroMQ context";
      return false;
    }

    void *candidate = zmq_socket(context, ZMQ_PUSH);
    if (candidate == nullptr) {
      last_failure_ = zmq_strerror(zmq_errno());
      return false;
    }

    const std::string monitor_endpoint =
        "inproc://spectrum-sender-monitor-" +
        std::to_string(reinterpret_cast<uintptr_t>(this));
    if (zmq_socket_monitor(candidate, monitor_endpoint.c_str(),
                           ZMQ_EVENT_CONNECTED |
                               ZMQ_EVENT_DISCONNECTED) != 0) {
      last_failure_ = zmq_strerror(zmq_errno());
      zmq_close(candidate);
      return false;
    }
    void *monitor = zmq_socket(context, ZMQ_PAIR);
    if (monitor == nullptr) {
      last_failure_ = zmq_strerror(zmq_errno());
      zmq_close(candidate);
      return false;
    }

    const int one = 1;
    const int zero = 0;
    // Keep at most one result per logical window in ZeroMQ. This bounds stale
    // data when the Writer stops and makes subsequent non-blocking sends drop.
    const int send_hwm = 1;
    const int heartbeat_ms = 1000;
    const int heartbeat_timeout_ms = 3000;
    const int reconnect_ms = 100;
    const int reconnect_max_ms = 1000;
    if (zmq_setsockopt(monitor, ZMQ_LINGER, &zero, sizeof(zero)) != 0 ||
        zmq_connect(monitor, monitor_endpoint.c_str()) != 0 ||
        zmq_setsockopt(candidate, ZMQ_IMMEDIATE, &one, sizeof(one)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_LINGER, &zero, sizeof(zero)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_SNDHWM, &send_hwm,
                       sizeof(send_hwm)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_SNDTIMEO, &zero, sizeof(zero)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_RECONNECT_IVL, &reconnect_ms,
                       sizeof(reconnect_ms)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_RECONNECT_IVL_MAX, &reconnect_max_ms,
                       sizeof(reconnect_max_ms)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_HEARTBEAT_IVL, &heartbeat_ms,
                       sizeof(heartbeat_ms)) != 0 ||
        zmq_setsockopt(candidate, ZMQ_HEARTBEAT_TIMEOUT,
                       &heartbeat_timeout_ms,
                       sizeof(heartbeat_timeout_ms)) != 0) {
      last_failure_ = zmq_strerror(zmq_errno());
      zmq_close(monitor);
      zmq_close(candidate);
      return false;
    }

    if (zmq_connect(candidate, endpoint_.c_str()) != 0) {
      last_failure_ = zmq_strerror(zmq_errno());
      zmq_close(monitor);
      zmq_close(candidate);
      return false;
    }
    socket_ = candidate;
    monitor_socket_ = monitor;
    owner_thread_ = std::this_thread::get_id();
    last_failure_.clear();
    return true;
  }

  void *socket_ = nullptr;
  void *monitor_socket_ = nullptr;
  std::thread::id owner_thread_{};
  bool monitor_connected_ = false;
  uint16_t server_id_ = 0;
  uint64_t next_sequence_ = 0;
  std::string endpoint_;
  std::string last_failure_;
};
