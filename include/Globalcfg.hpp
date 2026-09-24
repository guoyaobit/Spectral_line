#pragma once

#include "readerwritercircularbuffer.h"
#include "readerwriterqueue.h"
#include "spdlog/async.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"
#include <SpectrumSender.hpp>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
#include <thread>
#include <vdif.hpp>
#include <vector>
#include <yaml-cpp/yaml.h>
enum class ObservationMode : uint8_t {
  BASEBAND = 0, // Raw baseband recording
  SPECTRAL = 1, // Spectral-line observation
  CONTINUUM = 2 // Continuum observation
};
enum class BeamId : uint8_t {
  A = 0,
  B = 1,
};
struct Packet {
  uint8_t payload[8192]; // 4096*(Re + Im)
};
struct PacketBatch {
  int count;
  uint64_t batch_id = 0;          // packet_id / count
  uint32_t received_count = 0;    // unique packets actually received
  bool header_valid = false;      // hdrs[0] contains batch start time
  std::vector<uint64_t> pkt_id;
  std::vector<uint8_t> noise_state;
  bool valid = false;
  Packet *buffer; // Contiguous packet buffer
  std::vector<VDIF> hdrs;
  std::vector<Packet *> pkts; // Packets for one FFT interval
};

// Integrated result stored on the host.
struct StokesResult {
  size_t frame_id;         // Frame or integration interval
  std::vector<float> data; // One float4 (I, Q, U, V) per frequency bin
};
struct WindowConfig {
  // Actual FFT-bin-aligned frequency range. end_freq is exclusive.
  double start_freq;
  double end_freq;
  size_t start_idx;
  double center_freq;
  double BW;
  SpectrumSender sender;
  spectrum_header header;
  int port;
};
struct SubbandConfig {
  u_int8_t subband_id;
  bool enable = true;
  BeamId beam = BeamId::A;
  int gpu_id;
  double start_freq;
  double end_freq;
  static constexpr double BW = 256e6;
  int port;
  std::vector<WindowConfig *> windows;
};
struct NoiseSourceConfig {
  double period_ms = 1000.0;
  double duty_cycle = 50.0;
  double transition_blank_ms = 10.0;
};
class GlobalConfig {
public:
  static GlobalConfig &getInstance() {
    static GlobalConfig instance;
    return instance;
  }
  GlobalConfig(const GlobalConfig &) = delete;
  GlobalConfig &operator=(const GlobalConfig &) = delete;
  std::shared_ptr<spdlog::logger> logger_;
  std::mutex init_mutex;
  std::condition_variable init_cv;
  std::string receiver_name = "7mm";

  size_t ready_threads = 0;
  size_t total_threads = 0;
  void initlog() {
    // Generate a timestamped log filename.
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);

    std::ostringstream oss;
    oss << "log_" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".log";
    std::string logfile = oss.str();

    spdlog::init_thread_pool(1 << 16, 1); // 64K queue, one worker thread

    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(logfile, true);
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    std::vector<spdlog::sink_ptr> log_sinks{file_sink, stdout_sink};

    logger_ = std::make_shared<spdlog::async_logger>(
        "async_logger", log_sinks.begin(), log_sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    spdlog::set_default_logger(logger_);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_every(std::chrono::seconds(1));
    printf("Program started; log file: %s\n", logfile.c_str());
  }
  // Runtime defaults.
  bool Debug_mode = false;
  int ServerID = 0;
  uint8_t max_streams = 16;        // Maximum number of receive streams
  uint8_t enabled_streams = 0;     // Number of enabled receive streams
  const int sampling_rate = 256e6; // samaping rate
  std::string Storage_node_ip, Storage_node_mac, Sender_Nic;
  const int precision = 1 + 1;  // real 8bit ,image 8bit
  const int packet_size = 8192; // Payload bytes per packet
  int total_nfft = 65536;       // must be multipied by 4096
  const int Max_nfft = 65536 * 256;
  double win_bw = 256e6;
  int win_channels = 4096;
  double integration_t = 1;
  ObservationMode observation_mode = ObservationMode::SPECTRAL;
  bool cal_mode = false;
  NoiseSourceConfig noise_source;
  std::string Baseband_folder0;
  std::string Baseband_folder1;
  int Baseband_bits = 8;
  bool subband_monitor = false; // Publish recent frames for monitoring
  // Packets in one FFT interval.
  int batchsize() const { return total_nfft / 4096; }
  // Duration of one FFT interval.
  double fft_period() const {
    return static_cast<double>(total_nfft) / sampling_rate;
  }

  // Round the requested integration time down to complete FFT intervals.
  double integration_time() const {
    int N = static_cast<int>(integration_t / fft_period());
    if (N < 1)
      N = 1; // Always integrate at least one FFT.
    return N * fft_period();
  }
  // Input queues and backing memory.
  size_t Memory_pool_per_stream = 64; // GB
  Packet *packet_pool = nullptr;
  std::size_t QUEUE_CAPACITY = 64; // key value about memory usage
  std::vector<SubbandConfig *> subbands;
  struct StreamContext {
    bool enable = false;

    int subband_id = -1;

    int pool_index = -1;

    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> queue;

    std::vector<PacketBatch *> pool;

    StreamContext(size_t queue_capacity) : queue(queue_capacity) {}
  };
  std::vector<StreamContext> streams;
  ObservationMode parseObservationMode(const YAML::Node &node) {
    const int mode = node.as<int>();

    switch (mode) {
    case 0:
      return ObservationMode::BASEBAND;
    case 1:
      return ObservationMode::SPECTRAL;
    case 2:
      return ObservationMode::CONTINUUM;
    default:
      throw std::runtime_error(
          "Invalid observation_mode: " + std::to_string(mode) +
          " (valid values: 0, 1, 2)");
    }
  }
  // Load and validate the YAML configuration.
  bool initFromYaml(const std::string &filename) {
    try {
      YAML::Node config = YAML::LoadFile(filename);
      if (config["Debug"])
        Debug_mode = config["Debug"].as<bool>();
      if (!config["ServerID"] || !config["ServerID"].IsScalar())
        throw std::runtime_error("Configuration is missing ServerID");
      ServerID = config["ServerID"].as<int>();
      if (ServerID < 0 || ServerID > 7)
        throw std::runtime_error("ServerID must be between 0 and 7");
      if (config["Memory_pool_per_stream"])
        Memory_pool_per_stream =
            config["Memory_pool_per_stream"].as<size_t>(); // GB
      if (config["observation_mode"])
        observation_mode = parseObservationMode(config["observation_mode"]);
      if (config["Storage_node_ip"] && config["Storage_node_ip"].IsScalar()) {
        Storage_node_ip = config["Storage_node_ip"].as<std::string>();
      } else {
        Storage_node_ip = "127.0.0.1"; // Default value
      }
      if (config["Storage_node_mac"] && config["Storage_node_mac"].IsScalar()) {
        Storage_node_mac = config["Storage_node_mac"].as<std::string>();
      } else {
        throw std::runtime_error("Configuration is missing Storage_node_mac");
      }
      if (config["Sender_Nic"] && config["Sender_Nic"].IsScalar()) {
        Sender_Nic = config["Sender_Nic"].as<std::string>();
      } else {
        throw std::runtime_error("Configuration is missing Sender_Nic");
      }
      if (observation_mode == ObservationMode::BASEBAND) {
        if (config["Baseband_Folder0"] &&
            config["Baseband_Folder0"].IsScalar()) {
          Baseband_folder0 = config["Baseband_Folder0"].as<std::string>();
        } else {
          throw std::runtime_error("Configuration is missing Baseband_Folder0");
        }
        if (config["Baseband_Folder1"] &&
            config["Baseband_Folder1"].IsScalar()) {
          Baseband_folder1 = config["Baseband_Folder1"].as<std::string>();
        } else {
          throw std::runtime_error("Configuration is missing Baseband_Folder1");
        }
         if (config["Baseband_bits"] && config["Baseband_bits"].IsScalar()) {
           Baseband_bits = config["Baseband_bits"].as<int>();
         } else {
           throw std::runtime_error("Configuration is missing Baseband_bits");
         }
         if (Baseband_bits != 8 && Baseband_bits != 4 && Baseband_bits != 2) {
           throw std::runtime_error("Baseband_bits must be one of 8, 4, or 2");
         }

        namespace fs = std::filesystem;

        auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());

        std::tm tm;
        localtime_r(&now, &tm);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S");

        std::string datetime = oss.str();

        Baseband_folder0 = Baseband_folder0 + "/" + datetime;
        fs::create_directories(Baseband_folder0);

        Baseband_folder1 = Baseband_folder1 + "/" + datetime;
        fs::create_directories(Baseband_folder1);
      }
      if (config["subband_monitor"])
        subband_monitor = config["subband_monitor"].as<bool>();
      if (config["win_bw"])
        win_bw = config["win_bw"].as<double>();
      if (!std::isfinite(win_bw) || win_bw <= 0.0)
        throw std::runtime_error("win_bw must be finite and > 0");
      if (config["win_channels"])
        win_channels = config["win_channels"].as<int>();
      if (win_channels <= 0)
        throw std::runtime_error("win_channels must be > 0");
      // Derive the FFT length from the requested window.
      const double requested_nfft =
          static_cast<double>(sampling_rate) / win_bw * win_channels;
      if (!std::isfinite(requested_nfft) || requested_nfft < 1.0 ||
          requested_nfft > static_cast<double>(Max_nfft)) {
        throw std::runtime_error("Calculated FFT length is out of range");
      }
      total_nfft = static_cast<int>(std::llround(requested_nfft));

      if (config["integration_t"])
        integration_t = config["integration_t"].as<double>();
      if (config["cal_mode"])
        cal_mode = config["cal_mode"].as<bool>();
      if (config["noise_source"]) {
        const auto &ns = config["noise_source"];

        if (ns["period_ms"])
          noise_source.period_ms = ns["period_ms"].as<double>();

        if (ns["duty_cycle"])
          noise_source.duty_cycle = ns["duty_cycle"].as<double>();

        if (ns["transition_blank_ms"])
          noise_source.transition_blank_ms =
              ns["transition_blank_ms"].as<double>();
      }

      if (noise_source.period_ms <= 0.0)
        throw std::runtime_error("noise_source.period_ms must be > 0");

      if (noise_source.duty_cycle <= 0.0 || noise_source.duty_cycle >= 100.0)
        throw std::runtime_error(
            "noise_source.duty_cycle must be between 0 and 100");

      if (noise_source.transition_blank_ms < 0.0)
        throw std::runtime_error(
            "noise_source.transition_blank_ms must be >= 0");

      if (!config["subbands"] || !config["subbands"].IsSequence()) {
        throw std::runtime_error("Configuration is missing subbands");
      }
      int subband_id = 0;
      for (const auto &sbNode : config["subbands"]) {
        SubbandConfig *sb = new SubbandConfig();
        sb->subband_id = subband_id++;
        sb->enable = sbNode["enable"].as<bool>();
        std::string beam_name;
        if (!sbNode["beam"]) {
          throw std::runtime_error(
              "Every subband entry must specify beam A or B");
        } else if (!sbNode["beam"].IsScalar()) {
          throw std::runtime_error("Subband beam must be a scalar A or B");
        } else {
          beam_name = sbNode["beam"].as<std::string>();
          if (beam_name == "A") {
            sb->beam = BeamId::A;
          } else if (beam_name == "B") {
            sb->beam = BeamId::B;
          } else {
            throw std::runtime_error("Subband beam must be A or B");
          }
        }
        if (!sb->enable) {
          subbands.push_back(sb);
          continue;
        }
        sb->gpu_id = sbNode["gpu_id"].as<int>();
        sb->start_freq = sbNode["start_freq"].as<double>();
        sb->end_freq = sbNode["end_freq"].as<double>();
        sb->port = sbNode["port"].as<int>();
        if (sb->port <= 0 || sb->port > 65535)
          throw std::runtime_error(
              "Subband result destination port must be between 1 and 65535");

        if (!std::isfinite(sb->start_freq) ||
            !std::isfinite(sb->end_freq) ||
            sb->end_freq <= sb->start_freq) {
          throw std::runtime_error("Invalid subband frequency range");
        }
        const double subband_bw = sb->end_freq - sb->start_freq;
        if (std::abs(subband_bw - sampling_rate) > 0.5) {
          throw std::runtime_error(
              "Subband bandwidth must equal the 256 MHz sampling rate");
        }

        if (!sbNode["windows"] || !sbNode["windows"].IsSequence() ||
            sbNode["windows"].size() == 0 ||
            sbNode["windows"].size() > 4)
          throw std::runtime_error(
              "Each enabled subband requires between 1 and 4 windows");
        int win_id = 0;
        for (const auto &winNode : sbNode["windows"]) {
          WindowConfig *w = new WindowConfig();
          w->port = sb->port;
          w->center_freq = winNode["center_freq"].as<double>();
          if (!std::isfinite(w->center_freq))
            throw std::runtime_error("Window center frequency must be finite");

          const double requested_start_freq =
              w->center_freq - win_bw / 2.0;
          const double requested_end_freq =
              w->center_freq + win_bw / 2.0;

          if (requested_start_freq < sb->start_freq) {
            throw std::runtime_error("Window start fre < subband start fre!");
          }
          if (requested_end_freq > sb->end_freq) {
            throw std::runtime_error("Window end freq > subband end freq!");
          }

          const double channel_bw_hz =
              static_cast<double>(sampling_rate) / total_nfft;
          const long long start_idx = std::llround(
              (requested_start_freq - sb->start_freq) / channel_bw_hz);
          if (start_idx < 0 ||
              static_cast<unsigned long long>(start_idx) +
                      static_cast<unsigned long long>(win_channels) >
                  static_cast<unsigned long long>(total_nfft)) {
            throw std::runtime_error(
                "FFT-bin-aligned window exceeds subband bounds");
          }

          w->start_idx = static_cast<size_t>(start_idx);
          w->start_freq =
              sb->start_freq + w->start_idx * channel_bw_hz;
          w->BW = win_channels * channel_bw_hz;
          w->end_freq = w->start_freq + w->BW;
          w->sender.init(Storage_node_ip, w->port);
          w->header.exposure = integration_time();
          w->header.channel_bw_hz = channel_bw_hz;
          w->header.subband_start_freq = sb->start_freq;
          w->header.subband_end_freq = sb->end_freq;
          w->header.beam_id = static_cast<uint8_t>(sb->beam);
          // Every adjacent A/B pair represents one physical frequency
          // subband. beam_id distinguishes the two beams, while the global
          // subband ID spans 0-31 across GPU0-GPU7.
          w->header.subband_id =
              ServerID * 4 + sb->subband_id / 2;
          w->header.start_freq_hz = w->start_freq;
          w->header.n_channels = win_channels;
          w->header.stokes = 4;
          w->header.cal_mode = cal_mode ? 1 : 0;
          w->header.window_id = win_id++;
          logger_->info(
              "subband config {} -> global {} beam {} window {}: "
              "requested center {:.9f} MHz, "
              "output [{:.9f}, {:.9f}) MHz, channel width {:.9f} Hz, "
              "destination {}:{}",
              sb->subband_id, w->header.subband_id, beam_name,
              w->header.window_id, w->center_freq / 1e6,
              w->start_freq / 1e6, w->end_freq / 1e6, channel_bw_hz,
              Storage_node_ip, w->port);
          sb->windows.push_back(w);
        }

        subbands.push_back(sb);
      }

      if (subbands.size() != 8)
        throw std::runtime_error(
            "Exactly 8 subband/beam entries are required per server");

      // The static layout is four physical subbands per server. Each
      // physical subband is represented by adjacent A and B entries, which
      // must describe the same frequency range and window layout.
      for (size_t pair = 0; pair < 4; ++pair) {
        const auto *beam_a = subbands[pair * 2];
        const auto *beam_b = subbands[pair * 2 + 1];
        if (beam_a->beam != BeamId::A || beam_b->beam != BeamId::B) {
          throw std::runtime_error(
              "Subband entries must be ordered as A/B pairs");
        }
        if (beam_a->enable != beam_b->enable) {
          throw std::runtime_error(
              "Both beams in a physical subband must have the same enable state");
        }
        if (!beam_a->enable)
          continue;
        if (std::abs(beam_a->start_freq - beam_b->start_freq) > 0.5 ||
            std::abs(beam_a->end_freq - beam_b->end_freq) > 0.5) {
          throw std::runtime_error(
              "A/B entries in a physical subband must have the same frequency range");
        }
        if (beam_a->windows.size() != beam_b->windows.size()) {
          throw std::runtime_error(
              "A/B entries in a physical subband must have the same windows");
        }
        for (size_t window = 0; window < beam_a->windows.size(); ++window) {
          if (std::abs(beam_a->windows[window]->start_freq -
                       beam_b->windows[window]->start_freq) > 0.5 ||
              std::abs(beam_a->windows[window]->end_freq -
                       beam_b->windows[window]->end_freq) > 0.5) {
            throw std::runtime_error(
                "A/B entries in a physical subband must have matching window frequencies");
          }
        }
      }
    } catch (const std::exception &e) {
      logger_->error("Configuration parsing failed: {}", e.what());
      return false;
    }
    return checkConfig();
  }
  bool checkConfig() const {
    bool ok = true;

    if (win_channels <= 0) {
      logger_->error(" win_channels must be > 0");
      ok = false;
    }
    if (total_nfft % 4096 != 0) {
      logger_->error(" win_channels* 256e6/bw must be a multiple of 4096 "
                     "(current value: {}",
                     total_nfft);
      ok = false;
    }
    if (total_nfft < 65536) {
      logger_->error(" win_channels* 256e6/bw must >= 65536 {}", win_channels);
      ok = false;
    }
    return ok;
  }

private:
  GlobalConfig() {}
  ~GlobalConfig() = default;
};
