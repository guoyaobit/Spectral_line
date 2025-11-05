#include <Globalcfg.hpp>
#include <DoPFBFFT.hpp>
#include <cuda_runtime.h>
#include <iostream>
// #include <GpuStokes.h>
#include <complex>
#include <thread>
std::vector<std::unique_ptr<GpuPfbFft>> g_procspfbfft;
std::vector<std::thread> g_threadspfbfft;

// std::vector<std::unique_ptr<GpuStokes>> g_procstokes;
// std::vector<std::thread> g_threadstokes;

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

// void dopfb(std::unique_ptr<GpuPfbFft> proc)
// {
//     while (1)
//     {
//         proc->accumulate_one_block();
//         proc->submit_PFB_FFT();
//         proc->StokesAcc();
//     }
// }
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
    std::unique_ptr<GpuPfbFft> obj = std::make_unique<GpuPfbFft>(subband_id, pfbwin.data(), result);
    // for (size_t i = 0; i < num_objects; i++)
    // {
    //     objs.push_back(std::make_unique<GpuPfbFft>(subband_id, gpu_id, nfft, num_taps, pfbwin.data(),result,&shared_counter));
    // }

    // 循环处理流水线
    while (true)
    {
        // for (auto &obj : objs)
        // {
        obj->accumulate_one_block(); // CPU -> GPU 异步拷贝
        obj->submit_PFB_FFT();       // PFB+ FFT
        obj->StokesAcc();            // Stokes kernel

        // }
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
int init()
{
    auto &cfg = GlobalConfig::getInstance();

    int queue_capacity = cfg.QUEUE_CAPACITY;
    // two polar in one subband
    cfg.g_in_queues.reserve(cfg.recv_streams);
    for (size_t s = 0; s < cfg.recv_streams; s++)
    {
        cfg.g_in_queues.emplace_back(queue_capacity);
    }
    cfg.g_in_pools.resize(cfg.recv_streams);
    if (cfg.total_nfft % 4096 != 0)
        cfg.logger_->error("total_nfft must be multiplied by 4096!");

    int pool_size = cfg.QUEUE_CAPACITY;
    int batchsize = cfg.batchsize();
    cfg.logger_->debug(
        "Allocating pinned memory {} MiB", cfg.recv_streams * pool_size * sizeof(Packet) * batchsize / 1e6);
    for (size_t s = 0; s < cfg.recv_streams; ++s)
    {
        cfg.g_in_pools[s].resize(pool_size);
        for (size_t b = 0; b < pool_size; ++b)
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