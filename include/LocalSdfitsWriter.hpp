#pragma once

#include <SpectrumSender.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class sdfits;

// Asynchronous local SDFITS sink.  The worker uses the same sdfits class and
// FITS template as Spectral_Line_Recorder, so local and remote files have the
// same HDUs, keywords, columns, TDIM order, and float payload layout.
class LocalSdfitsWriter {
public:
  static LocalSdfitsWriter &instance();

  LocalSdfitsWriter(const LocalSdfitsWriter &) = delete;
  LocalSdfitsWriter &operator=(const LocalSdfitsWriter &) = delete;

  void configure(bool enabled, std::string output_folder,
                 size_t max_queue_bytes = 2048ULL * 1024ULL * 1024ULL);
  bool enqueue(const spectrum_header &header, const void *payload,
               size_t payload_bytes);
  void shutdown();

private:
  LocalSdfitsWriter() = default;
  ~LocalSdfitsWriter();

  struct Job {
    spectrum_header header{};
    std::vector<float> data;
  };

  struct BandKey {
    uint16_t subband_id;
    uint16_t window_id;
    uint8_t beam_id;
    double start_freq_hz;
    double channel_bw_hz;
    uint32_t n_channels;

    bool operator==(const BandKey &other) const noexcept;
  };

  struct BandKeyHash {
    size_t operator()(const BandKey &key) const noexcept;
  };

  void run();
  void write_job(Job &job);
  sdfits *get_or_create_writer(const spectrum_header &header);

  std::atomic<bool> enabled_{false};
  bool stopping_ = false;
  std::string output_folder_;
  size_t max_queue_bytes_ = 0;
  size_t queued_bytes_ = 0;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> queue_;
  std::thread worker_;
  std::unordered_map<BandKey, std::unique_ptr<sdfits>, BandKeyHash> writers_;
};
