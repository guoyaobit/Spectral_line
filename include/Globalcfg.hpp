#pragma once

#include "readerwritercircularbuffer.h"
#include "readerwriterqueue.h"
#include "spdlog/async.h"
#include "spdlog/sinks/stdout_color_sinks.h"
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
  BASEBAND = 0, // 基带记录模式 (Raw Baseband Recording)
  SPECTRAL = 1, // 谱线观测模式 (Spectral Line Observation)
  CONTINUUM = 2 // 连续谱观测模式 (Continuum Observation)
};
struct Packet {
  uint8_t payload[8192]; // 4096*(Re + Im)
};
struct PacketBatch {
  int count;
  std::vector<uint> pkt_id;
  std::vector<uint8_t> noise_state;
  bool valid = true;
  Packet *buffer; // 连续大 buffer
  std::vector<VDIF> hdrs;
  std::vector<Packet *> pkts; // 一次 FFT 的数据包集合
};

// 最终结果存储结构
struct StokesResult {
  size_t frame_id;         // 哪一帧/积累段
  std::vector<float> data; // Nfft 个频点，每个频点一个 float4(I,Q,U,V)
};
struct WindowConfig {
  float start_freq;
  size_t start_idx;
  // float end_freq;
  // size_t end_idx;
  float center_freq;
  float BW;
  SpectrumSender sender;
  spectrum_header header;
  int port;
};
struct SubbandConfig {
  u_int8_t subband_id;
  bool enable = true;
  int gpu_id;
  float start_freq;
  float end_freq;
  static constexpr float BW = 256e6f;
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
    // 生成带时间戳的日志文件名
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);

    std::ostringstream oss;
    oss << "log_" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".log";
    std::string logfile = oss.str();

    // 初始化 spdlog 线程池（队列大小、线程数）
    spdlog::init_thread_pool(1 << 16, 1); // 64K 队列，1 个后台线程

    // 创建文件 sink
    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(logfile, true);

    // 创建异步 logger
    logger_ = std::make_shared<spdlog::async_logger>(
        "async_logger", file_sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    // 设置为默认 logger
    spdlog::set_default_logger(logger_);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_every(std::chrono::seconds(1));
    printf("程序启动，日志文件: %s\n", logfile.c_str());
  }
  // default para
  bool Debug_mode = false;
  int ServerID = 0;
  uint8_t max_streams = 16;        // 最大接收流数
  uint8_t enabled_streams = 0;     // 实际启用的接收流数
  const int sampling_rate = 256e6; // samaping rate
  std::string Storage_node_ip, Storage_node_mac, Sender_Nic;
  const int precision = 1 + 1;  // real 8bit ,image 8bit
  const int packet_size = 8192; // 每个数据包字节数
  int total_nfft = 65536;       // must be multipied by 4096
  const int Max_nfft = 65536 * 256;
  float win_bw = 256e6;
  int win_channels = 4096;
  double integration_t = 1;
  ObservationMode observation_mode =
      ObservationMode::SPECTRAL; // 默认单窗口分子谱线模式
  bool cal_mode = false;
  NoiseSourceConfig noise_source;
  std::string Baseband_folder0;
  std::string Baseband_folder1;
  int Baseband_bits = 8;
  bool subband_monitor = false; // 是否开启子频段监控模式
  // how many packet in one batch(FFT period)
  int batchsize() const { return total_nfft / 4096; }
  // 每次 FFT 的时间长度
  double fft_period() const {
    return static_cast<double>(total_nfft) / sampling_rate;
  }

  // 总积分时间，调整为 FFT 周期整数倍
  double integration_time() const {
    int N = static_cast<int>(integration_t / fft_period());
    if (N < 1)
      N = 1; // 至少 1 个 FFT
    return N * fft_period();
  }
  // input queques
  size_t Memory_pool_per_stream = 64; // GB
  Packet *packet_pool = nullptr;
  std::size_t QUEUE_CAPACITY = 64; // key value about memory usage
  // std::vector<moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *>>
  // stream_queues; std::vector<std::vector<PacketBatch *>> stream_pools;
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
  // 初始化 YAML 配置
  bool initFromYaml(const std::string &filename) {
    try {
      YAML::Node config = YAML::LoadFile(filename);
      if (config["Debug"])
        Debug_mode = config["Debug"].as<bool>();
      if (config["ServerID"])
        ServerID = config["ServerID"].as<int>();
      if (config["Memory_pool_per_stream"])
        Memory_pool_per_stream =
            config["Memory_pool_per_stream"].as<size_t>(); // GB
      if (config["observation_mode"])
        observation_mode = parseObservationMode(config["observation_mode"]);
      if (config["Storage_node_ip"] && config["Storage_node_ip"].IsScalar()) {
        Storage_node_ip = config["Storage_node_ip"].as<std::string>();
      } else {
        Storage_node_ip = "127.0.0.1"; // 默认值
      }
      if (config["Storage_node_mac"] && config["Storage_node_mac"].IsScalar()) {
        Storage_node_mac = config["Storage_node_mac"].as<std::string>();
      } else {
        throw std::runtime_error("配置文件缺少 Storage_node_mac!");
      }
      if (config["Sender_Nic"] && config["Sender_Nic"].IsScalar()) {
        Sender_Nic = config["Sender_Nic"].as<std::string>();
      } else {
        throw std::runtime_error("配置文件缺少 Sender_Nic!");
      }
      if (observation_mode == ObservationMode::BASEBAND) {
        if (config["Baseband_Folder0"] &&
            config["Baseband_Folder0"].IsScalar()) {
          Baseband_folder0 = config["Baseband_Folder0"].as<std::string>();
        } else {
          throw std::runtime_error("配置文件缺少 Baseband_Folder0!");
        }
        if (config["Baseband_Folder1"] &&
            config["Baseband_Folder1"].IsScalar()) {
          Baseband_folder1 = config["Baseband_Folder1"].as<std::string>();
        } else {
          throw std::runtime_error("配置文件缺少 Baseband_Folder1!");
        }
         if (config["Baseband_bits"] && config["Baseband_bits"].IsScalar()) {
           Baseband_bits = config["Baseband_bits"].as<int>();
         } else {
           throw std::runtime_error("配置文件缺少 Baseband_bits!");
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
          win_bw = config["win_bw"].as<float>();
        if (!std::isfinite(win_bw) || win_bw <= 0.0f)
          throw std::runtime_error("win_bw must be finite and > 0");
      if (config["win_channels"])
        win_channels = config["win_channels"].as<int>();
      // get total nfft from para

      total_nfft = sampling_rate / win_bw * win_channels;

      // printf("%d\n", total_nfft);

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
        throw std::runtime_error("配置文件缺少 subbands config!");
      }
      // int send_start_port = 60000;
      int subband_id = 0;
      for (const auto &sbNode : config["subbands"]) {
        SubbandConfig *sb = new SubbandConfig();
        sb->subband_id = subband_id++;
        sb->enable = sbNode["enable"].as<bool>();
        if (!sb->enable) {
          subbands.push_back(sb);
          continue;
        }
        sb->gpu_id = sbNode["gpu_id"].as<int>();
        sb->start_freq = sbNode["start_freq"].as<float>();
        sb->end_freq = sbNode["end_freq"].as<float>();
        sb->port = sbNode["port"].as<int>();

        if (!sbNode["windows"]) {
          throw std::runtime_error("配置文件缺少 window config!");
        }
        int win_id = 0;
        for (const auto &winNode : sbNode["windows"]) {
          WindowConfig *w = new WindowConfig();
          w->port = sb->port;
          w->center_freq = winNode["center_freq"].as<float>();

          w->BW = win_bw;
          w->start_freq = w->center_freq - win_bw / 2;

          if (w->start_freq < sb->start_freq) {
            throw std::runtime_error("Window start fre < subband start fre!");
          }
          const float end_freq = w->center_freq + win_bw / 2;
          if (end_freq > sb->end_freq) {
            throw std::runtime_error("Window end freq > subband end freq!");
          }

          w->start_idx =
              round(w->start_freq - sb->start_freq) / sb->BW * total_nfft;
          // w->end_idx = round(w->end_freq - sb->start_freq) / sb->BW *
          // total_nfft; if (w->end_idx - w->start_idx+1 != win_channels)
          // {
          //     logger_->error("YAML : cal start_idx or end_idx in windows
          //     error ! {} != {}", w->end_idx - w->start_idx, win_channels);
          // }
          w->sender.init(Storage_node_ip, w->port);
          w->header.exposure = integration_time();
          w->header.channel_bw_hz = (float)sampling_rate / total_nfft;
          w->header.subband_start_freq = sb->start_freq;
          w->header.subband_end_freq = sb->end_freq;
          // max 8 subbands in one server, so subband_id = ServerID*100
          w->header.subband_id = ServerID * 8 + sb->subband_id;
          w->header.start_freq_hz =
              sb->start_freq + w->start_idx * sampling_rate / total_nfft;
          w->header.n_channels = win_channels;
          w->header.window_id = win_id++;
          // send_start_port+=1;
          logger_->info("dest ip {},port {}", Storage_node_ip, w->port);
          // logger_->info("window start_idx = {} , window end_idx = {},
          // w->end_idx-w->start_idx = {}",
          //               w->start_idx, w->end_idx, w->end_idx - w->start_idx);
          // std::cout<<w->start_idx<<" to " <<w->end_idx <<" == "<<
          // w->end_idx-w->start_idx<<std::endl; w->BW =
          // winNode["BW"].as<float>();
          sb->windows.push_back(w);
        }

        if (sb->windows.size() > 4) {
          throw std::runtime_error("Each subband supports at most 4 windows");
        }
        subbands.push_back(sb);
      }
      // return true;
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
