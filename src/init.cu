#include <Globalcfg.hpp>
#include <DoPFBFFT.hpp>
#include <cuda_runtime.h>
#include <iostream>
// #include <GpuStokes.h>
#include <complex>
#include <thread>
#include <omp.h>
#include <baseband.hpp>

PacketBatch *allocatePacketBatch(size_t numpkts)
{
    PacketBatch *batch = new PacketBatch;
    batch->count = numpkts;

    cudaError_t err = cudaMallocHost((void **)&batch->buffer, sizeof(Packet) * numpkts);
    if (err != cudaSuccess)
    {
        std::cerr << "cudaMallocHost failed: " << cudaGetErrorString(err) << std::endl;
        delete batch;
        return nullptr;
    }
    memset(batch->buffer, 0, sizeof(Packet) * numpkts);
    batch->pkts.resize(numpkts);
    batch->pkt_id.resize(numpkts);
    batch->noise_state.resize(numpkts);
    batch->timestamps.resize(numpkts);
    for (size_t i = 0; i < numpkts; i++)
    {
        batch->pkts[i] = &batch->buffer[i];
    }
    return batch;
}

void freeDataBatch(PacketBatch *batch)
{
    if (!batch)
        return;
    cudaFreeHost(batch->buffer);
    delete batch;
}

void subband_thread(int subband_id,
                    std::vector<float> pfbwin)
{
    float4 *result;
    auto &cfg = GlobalConfig::getInstance();
    

    cudaSetDevice(cfg.subbands[subband_id]->gpu_id);

    cudaMalloc((void **)&result, cfg.total_nfft * sizeof(float4)); //
    cudaMemset(result, 0, cfg.total_nfft * sizeof(float4));
    int shared_counter = 0;
    // 线程内部创建多个对象
    
    if(cfg.observation_mode == 0) // baseband mode
    {
        cfg.logger_->info("subband {}: baseband mode, no PFB window generated", subband_id);
        std::unique_ptr<baseband> baseband_obj = std::make_unique<baseband>(subband_id);
        while (true)
        {
            baseband_obj->collectdata();
        }
    }
    else if (cfg.observation_mode == 1) // spectrum line mode
    {
        cfg.logger_->info("subband {}: spectrum line mode, PFB window generated", subband_id);
        std::unique_ptr<GpuPfbFft> obj = std::make_unique<GpuPfbFft>(subband_id, pfbwin.data(), result);
        while (true)
        {
            obj->accumulate_one_block(); // CPU -> GPU 异步拷贝
            obj->submit_PFB_FFT();       // PFB+ FFT
            obj->StokesAcc();            // Stokes kernel
        }
    }
    else
    {
        cfg.logger_->error("subband {}: unknown observation mode {}", subband_id, cfg.observation_mode);
        return;
    }
}

// 生成长度 N 的 sinc 窗函数 (归一化), 存到 win
void genPfbWin(std::vector<float> &win, int N, int P)
{
    if ((int)win.size() < N)
    {
        throw std::runtime_error("win size too small!");
    }

    float center = 1.0f * (N - 1) / 2.0f;
    float sum = 0.0f;

    // 生成 sinc 窗
    for (int i = 0; i < N; i++)
    {
        float t = i - center;
        float val;
        if (fabs(t) < 1e-6f)
        {
            val = 1.0f;
        }
        else
        {
            val = sinf(M_PI * t * P / N) / (M_PI * t * P / N);
        }
        win[i] = val;
        sum += val;
    }
    // 归一化
    for (int i = 0; i < N; i++)
    {
        win[i] /= sum;
    }
}
size_t calc_pool_size()
{
    constexpr uint64_t MAX_POOL_MEMORY = 64ULL * 1024 * 1024 * 1024; // 4 GiB
    auto &cfg = GlobalConfig::getInstance();
    size_t batch_bytes =
        cfg.recv_streams*sizeof(Packet) * cfg.batchsize();

    size_t pool_size =
        MAX_POOL_MEMORY / batch_bytes;
    return std::max<size_t>(pool_size, 2);
}
int init()
{
    auto &cfg = GlobalConfig::getInstance();
    cfg.QUEUE_CAPACITY = calc_pool_size();
    cfg.logger_->info("QUEUE_CAPACITY = {}", cfg.QUEUE_CAPACITY);
    // printf("QUEUE_CAPACITY = %zu\n", cfg.QUEUE_CAPACITY);
    // two polar in one subband
    cfg.g_in_queues.reserve(cfg.recv_streams);
    for (size_t s = 0; s < cfg.recv_streams; s++)
    {
        cfg.g_in_queues.emplace_back(cfg.QUEUE_CAPACITY);
    }
    cfg.g_in_pools.resize(cfg.recv_streams);
    if (cfg.total_nfft % 4096 != 0)
        cfg.logger_->error("total_nfft must be multiplied by 4096!");
    int batchsize = cfg.batchsize();
    cfg.logger_->debug(
        "Allocating pinned memory {} MiB", cfg.recv_streams * cfg.QUEUE_CAPACITY * sizeof(Packet) * batchsize / 1e6);
    for (size_t s = 0; s < cfg.recv_streams; ++s)
    {
        cfg.g_in_pools[s].resize(cfg.QUEUE_CAPACITY);
        
        for (size_t b = 0; b < cfg.QUEUE_CAPACITY; ++b)
        {
            cfg.g_in_pools[s][b] = allocatePacketBatch(batchsize);
        }
    }
    // PfB+ FFT para
    int Nfft = cfg.total_nfft;
    size_t num_taps = 4;
    std::vector<float> pfbwin(num_taps * Nfft);
    genPfbWin(pfbwin, num_taps * Nfft, num_taps);
    // init subband thread
    for (size_t i = 0; i < cfg.subbands.size(); i++)
    {
        std::thread t(subband_thread, i, pfbwin);
        t.detach();
    }
    return 0;
}