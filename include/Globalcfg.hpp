#pragma once

#include <vector>
#include <memory>
#include <thread>
#include "readerwriterqueue.h"
#include "readerwritercircularbuffer.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
// #include "spdlog/sinks/daily_file_sink.h"
// #include <spdlog/stopwatch.h>
#include "spdlog/sinks/stdout_color_sinks.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <iomanip>
#include <SpectrumSender.hpp>
struct Packet
{
    uint8_t payload[8192]; // 4096*(Re + Im)
};
struct PacketBatch
{
    int count;
    std::vector<uint> pkt_id;
    std::vector<uint8_t> noise_state;
    bool vaild = true;
    std::vector<uint64_t> timestamps;
    Packet *buffer;             // 连续大 buffer
    std::vector<Packet *> pkts; // 一次 FFT 的数据包集合
};

// 最终结果存储结构
struct StokesResult
{
    size_t frame_id;         // 哪一帧/积累段
    std::vector<float> data; // Nfft 个频点，每个频点一个 float4(I,Q,U,V)
};
struct WindowConfig
{
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
struct SubbandConfig
{
    int gpu_id;
    float start_freq;
    float end_freq;
    static constexpr float BW = 256e6f;
    int port;
    std::vector<WindowConfig *> windows;
};
class GlobalConfig
{
public:
    static GlobalConfig &getInstance()
    {
        static GlobalConfig instance;
        return instance;
    }
    GlobalConfig(const GlobalConfig &) = delete;
    GlobalConfig &operator=(const GlobalConfig &) = delete;
    std::shared_ptr<spdlog::logger> logger_;
    void initlog()
    {
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
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logfile, true);

        // 创建异步 logger
        logger_ = std::make_shared<spdlog::async_logger>(
            "async_logger",
            file_sink,
            spdlog::thread_pool(),
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
    int recv_streams = 16;
    const int sampling_rate = 256e6; // samaping rate
    std::string Storage_node_ip, Storage_node_mac, Sender_Nic;
    const int precision = 1 + 1;  // real 8bit ,image 8bit
    const int packet_size = 8192; // 每个数据包字节数
    int total_nfft = 65536;       // must be multipied by 4096
    const int Max_nfft = 65536 * 256;
    float win_bw = 256e6;
    int win_channels = 4096;
    double integration_t = 1;
    int observation_mode = 1; // 默认单窗口分子谱线模式
    bool cal_mode = false;

    // how many packet in one batch
    int batchsize() const { return total_nfft / 4096; }
    // 每次 FFT 的时间长度
    double fft_period() const
    {
        return static_cast<double>(total_nfft) / sampling_rate;
    }

    // 总积分时间，调整为 FFT 周期整数倍
    double integration_time() const
    {
        int N = static_cast<int>(integration_t / fft_period());
        if (N < 1)
            N = 1; // 至少 1 个 FFT
        return N * fft_period();
    }
    // input queques
    std::size_t QUEUE_CAPACITY = 64; // key value about memory usage
    std::vector<moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *>> g_in_queues;
    std::vector<std::vector<PacketBatch *>> g_in_pools;
    std::vector<SubbandConfig *> subbands;
    // 初始化 YAML 配置
    bool initFromYaml(const std::string &filename)
    {
        try
        {
            YAML::Node config = YAML::LoadFile(filename);
            if (config["Debug"])
                Debug_mode = config["Debug"].as<bool>();
            if (config["recv_streams"])
                recv_streams = config["recv_streams"].as<int>();

            if (config["Storage_node_ip"] && config["Storage_node_ip"].IsScalar())
            {
                Storage_node_ip = config["Storage_node_ip"].as<std::string>();
            }
            else
            {
                Storage_node_ip = "127.0.0.1"; // 默认值
            }
            if (config["Storage_node_mac"] && config["Storage_node_mac"].IsScalar())
            {
                Storage_node_mac = config["Storage_node_mac"].as<std::string>();
            }
            else
            {
                throw std::runtime_error("配置文件缺少 Storage_node_mac!");
            }
            if (config["Sender_Nic"] && config["Sender_Nic"].IsScalar())
            {
                Sender_Nic = config["Sender_Nic"].as<std::string>();
            }
            else
            {
                throw std::runtime_error("配置文件缺少 Sender_Nic!");
            }

            if (config["win_bw"])
                win_bw = config["win_bw"].as<float>();

            // std::cout<< win_bw<<std::endl;
            if (config["win_channels"])
                win_channels = config["win_channels"].as<int>();
            // get total nfft from para

            total_nfft = sampling_rate / win_bw * win_channels;

            // printf("%d\n", total_nfft);

            if (config["queue_capacity"])
                QUEUE_CAPACITY = config["queue_capacity"].as<std::size_t>();
            if (config["observation_mode"])
                observation_mode = config["observation_mode"].as<int>();
            if (config["integration_t"])
                integration_t = config["integration_t"].as<double>();
            if (config["cal_mode"])
                cal_mode = config["cal_mode"].as<bool>();
            if (!config["subbands"] || !config["subbands"].IsSequence())
            {
                throw std::runtime_error("配置文件缺少 subbands config!");
            }
            // int send_start_port = 60000;
            for (const auto &sbNode : config["subbands"])
            {
                SubbandConfig *sb = new SubbandConfig();
                sb->gpu_id = sbNode["gpu_id"].as<int>();
                // sb->A_port = sbNode["A_port"].as<int>();
                // sb->B_port = sbNode["B_port"].as<int>();
                sb->start_freq = sbNode["start_freq"].as<float>();
                sb->end_freq = sbNode["end_freq"].as<float>();
                sb->port = sbNode["port"].as<int>();

                if (!sbNode["windows"])
                {
                    throw std::runtime_error("配置文件缺少 window config!");
                }
                int win_id = 0;
                for (const auto &winNode : sbNode["windows"])
                {
                    WindowConfig *w = new WindowConfig();
                    w->port = sb->port;
                    w->center_freq = winNode["center_freq"].as<float>();

                    w->BW = win_bw;
                    w->start_freq = w->center_freq - win_bw / 2;

                    if (w->start_freq < sb->start_freq)
                    {
                        throw std::runtime_error("Window start fre < subband start fre!");
                    }
                    // w->end_freq = w->center_freq + win_bw / 2;
                    // if (w->end_freq > sb->end_freq)
                    // {
                    //     throw std::runtime_error("Window end fre > subband end fre!");
                    // }

                    w->start_idx = round(w->start_freq - sb->start_freq) / sb->BW * total_nfft;
                    // w->end_idx = round(w->end_freq - sb->start_freq) / sb->BW * total_nfft;
                    // if (w->end_idx - w->start_idx+1 != win_channels)
                    // {
                    //     logger_->error("YAML : cal start_idx or end_idx in windows error ! {} != {}", w->end_idx - w->start_idx, win_channels);
                    // }
                    w->sender.init(Storage_node_ip, w->port);
                    w->header.channel_bw_hz = (float)sampling_rate / total_nfft;
                    w->header.subband_start_freq = sb->start_freq;
                    w->header.subband_end_freq = sb->end_freq;
                    w->header.start_freq_hz = sb->start_freq + w->start_idx * sampling_rate / total_nfft;
                    w->header.n_channels = win_channels;
                    w->header.window_id = win_id++;
                    // send_start_port+=1;
                    logger_->info("dest ip {},port {}", Storage_node_ip, w->port);
                    // logger_->info("window start_idx = {} , window end_idx = {}, w->end_idx-w->start_idx = {}",
                    //               w->start_idx, w->end_idx, w->end_idx - w->start_idx);
                    // std::cout<<w->start_idx<<" to " <<w->end_idx <<" == "<< w->end_idx-w->start_idx<<std::endl;
                    // w->BW = winNode["BW"].as<float>();
                    sb->windows.push_back(w);
                }

                if (sb->windows.size() > 4)
                {
                    logger_->error("每个子带最多只能配置 4 个窗口!");
                }
                subbands.push_back(sb);
            }
            // return true;
        }
        catch (const YAML::Exception &e)
        {
            logger_->error("YAML 配置解析失败: ", e.what());
            return false;
        }
        return checkConfig();
    }
    bool checkConfig() const
    {
        bool ok = true;
        if (recv_streams <= 0)
        {
            logger_->error(" recv_streams must be > 0");
            ok = false;
        }

        if (win_channels <= 0)
        {
            logger_->error(" win_channels must be > 0");
            ok = false;
        }
        if (total_nfft % 4096 != 0)
        {
            logger_->error(" win_channels* 256e6/bw must be a multiple of 4096 (current value: {}", total_nfft);
            ok = false;
        }
        if (total_nfft < 65536)
        {
            logger_->error(" win_channels* 256e6/bw must >= 65536 {}", win_channels);
            ok = false;
        }

        if (QUEUE_CAPACITY < 64)
        {
            logger_->error(" queue_capacity must be > 32");
            ok = false;
        }
        if (observation_mode < 1 || observation_mode > 3)
        {
            logger_->error(" observation_mode must be 1, 2, or 3");
            ok = false;
        }
        return ok;
    }

private:
    GlobalConfig() {}
    ~GlobalConfig() = default;
};
