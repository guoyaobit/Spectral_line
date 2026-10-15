#include <LocalSdfitsWriter.hpp>

#include <Globalcfg.hpp>
#include <sdfits.h>
#include <SdfitsMetadata.hpp>
#include <time_utils.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <utility>

LocalSdfitsWriter &LocalSdfitsWriter::instance() {
  static LocalSdfitsWriter writer;
  return writer;
}

bool LocalSdfitsWriter::BandKey::operator==(
    const BandKey &other) const noexcept {
  return subband_id == other.subband_id && window_id == other.window_id &&
         beam_id == other.beam_id && start_freq_hz == other.start_freq_hz &&
         channel_bw_hz == other.channel_bw_hz &&
         n_channels == other.n_channels;
}

size_t LocalSdfitsWriter::BandKeyHash::operator()(
    const BandKey &key) const noexcept {
  size_t value = std::hash<uint16_t>{}(key.subband_id);
  const auto combine = [&value](size_t next) {
    value ^= next + 0x9e3779b9U + (value << 6) + (value >> 2);
  };
  combine(std::hash<uint16_t>{}(key.window_id));
  combine(std::hash<uint8_t>{}(key.beam_id));
  combine(std::hash<double>{}(key.start_freq_hz));
  combine(std::hash<double>{}(key.channel_bw_hz));
  combine(std::hash<uint32_t>{}(key.n_channels));
  return value;
}

void LocalSdfitsWriter::configure(bool enabled, std::string output_folder,
                                  size_t max_queue_bytes) {
  if (!enabled)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (enabled_.load(std::memory_order_acquire))
    throw std::runtime_error("LocalSdfitsWriter is already configured");
  if (output_folder.empty())
    throw std::runtime_error("Local SDFITS output folder is empty");
  if (max_queue_bytes == 0)
    throw std::runtime_error("Local SDFITS queue size must be greater than zero");

  std::filesystem::create_directories(output_folder);
  output_folder_ = std::move(output_folder);
  max_queue_bytes_ = max_queue_bytes;
  stopping_ = false;
  enabled_.store(true, std::memory_order_release);
  worker_ = std::thread(&LocalSdfitsWriter::run, this);
}

bool LocalSdfitsWriter::enqueue(const spectrum_header &header,
                                const void *payload, size_t payload_bytes) {
  if (!enabled_.load(std::memory_order_acquire))
    return true;
  if (payload == nullptr || payload_bytes == 0 ||
      payload_bytes % sizeof(float) != 0)
    return false;

  Job job;
  job.header = header;
  const float *begin = static_cast<const float *>(payload);
  job.data.assign(begin, begin + payload_bytes / sizeof(float));

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queued_bytes_ + payload_bytes > max_queue_bytes_)
      return false;
    queued_bytes_ += payload_bytes;
    queue_.push_back(std::move(job));
  }
  ready_.notify_one();
  return true;
}

sdfits *LocalSdfitsWriter::get_or_create_writer(
    const spectrum_header &header) {
  const BandKey key{header.subband_id, header.window_id, header.beam_id,
                    header.start_freq_hz, header.channel_bw_hz,
                    header.n_channels};
  const auto existing = writers_.find(key);
  if (existing != writers_.end())
    return existing->second.get();

  auto writer = std::make_unique<sdfits>();
  writer->new_file = 1;
  const double stop_freq_hz =
      header.start_freq_hz + header.channel_bw_hz * header.n_channels;
  const char beam_name = header.beam_id == 0 ? 'A' : 'B';
  const int filename_length = std::snprintf(
      writer->basefilename, sizeof(writer->basefilename),
      "%s/sb%02u_w%u_%.2f-%.2fMHz_%c_%s_%u", output_folder_.c_str(),
      static_cast<unsigned>(header.subband_id),
      static_cast<unsigned>(header.window_id), header.start_freq_hz / 1e6,
      stop_freq_hz / 1e6, beam_name,
      GlobalConfig::getInstance().source_on ? "ON" : "OFF",
      static_cast<unsigned>(header.n_channels));
  if (filename_length < 0 ||
      static_cast<size_t>(filename_length) >= sizeof(writer->basefilename)) {
    GlobalConfig::getInstance().logger_->error(
        "Local SDFITS filename is too long for subband {}, window {}",
        header.subband_id, header.window_id);
    return nullptr;
  }

  writer->hdr.nchan = header.n_channels;
  const uint64_t start_ns = header.integration_start_ns;
  if (!spectrum_time::format_fits_utc(start_ns, writer->hdr.date_obs,
                                      sizeof(writer->hdr.date_obs)))
    return nullptr;
  writer->hdr.sttmjd = spectrum_time::unix_ns_to_mjd(start_ns);
  writer->hdr.hwexposr = header.exposure;
  writer->hdr.chan_bw = header.channel_bw_hz;
  writer->hdr.obsfreq = header.start_freq_hz +
                        header.channel_bw_hz * header.n_channels / 2;
  writer->hdr.nsubband = 1;
  writer->hdr.npol = 4;
  configure_sdfits_metadata(*writer, GlobalConfig::getInstance().sdfits_metadata,
                            GlobalConfig::getInstance().object,
                            header.cal_mode != 0,
                            GlobalConfig::getInstance().source_on);
  const int status = writer->sdfits_create();
  if (status != 0) {
    GlobalConfig::getInstance().logger_->error(
        "Failed to create local SDFITS for subband {}, window {}, status={}",
        header.subband_id, header.window_id, status);
    return nullptr;
  }
  GlobalConfig::getInstance().logger_->info("Created local SDFITS: {}",
                                            writer->filename);
  sdfits *result = writer.get();
  writers_.emplace(key, std::move(writer));
  return result;
}

void LocalSdfitsWriter::write_job(Job &job) {
  sdfits *writer = get_or_create_writer(job.header);
  if (writer == nullptr)
    return;
  writer->data_columns.data =
      reinterpret_cast<unsigned char *>(job.data.data());
  writer->data_columns.cal_on = job.header.noise_state;
  writer->data_columns.integ_num =
      static_cast<int>(job.header.integration_id);
  writer->data_columns.centre_freq[0] = writer->hdr.obsfreq;
  writer->data_columns.time = spectrum_time::unix_ns_to_mjd(
      job.header.integration_start_ns);
  writer->data_columns.exposure = job.header.exposure;
  const int status = writer->sdfits_write_subint();
  writer->data_columns.data = nullptr;
  if (status != 0)
    GlobalConfig::getInstance().logger_->error(
        "Failed to write local SDFITS row to {}, status={}", writer->filename,
        status);
}

void LocalSdfitsWriter::run() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_)
        break;
      job = std::move(queue_.front());
      queued_bytes_ -= job.data.size() * sizeof(float);
      queue_.pop_front();
    }
    write_job(job);
  }
  for (auto &entry : writers_)
    entry.second->sdfits_close();
  writers_.clear();
}

void LocalSdfitsWriter::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_.load(std::memory_order_acquire))
      return;
    stopping_ = true;
  }
  ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
  enabled_.store(false, std::memory_order_release);
}

LocalSdfitsWriter::~LocalSdfitsWriter() { shutdown(); }
