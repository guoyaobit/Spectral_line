#pragma once
#include <cuda_runtime.h>
#include <cufft.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <iostream>
// #include <syncstream>
#include <inttypes.h>
#include <VDIF.hpp>
#include <Globalcfg.hpp>
#include <cuda_fp16.h>
#include <fstream>
#include <iostream>
#include <string>
#include <future>
#include <Writer.h>

#define CUDA_CHECK(call)                                                                                                \
    do                                                                                                                  \
    {                                                                                                                   \
        cudaError_t _e = (call);                                                                                        \
        if (_e != cudaSuccess)                                                                                          \
        {                                                                                                               \
            std::cerr << "CUDA error " << cudaGetErrorString(_e) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            throw std::runtime_error(cudaGetErrorString(_e));                                                           \
        }                                                                                                               \
    } while (0)

#define CUFFT_CHECK(call)                                                                            \
    do                                                                                               \
    {                                                                                                \
        cufftResult _r = (call);                                                                     \
        if (_r != CUFFT_SUCCESS)                                                                     \
        {                                                                                            \
            std::cerr << "cuFFT error " << _r << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            throw std::runtime_error("cuFFT error");                                                 \
        }                                                                                            \
    } while (0)

// each VDIF packet bytes and samples
static constexpr size_t PKT_DATA_BYTES = 8192;
static constexpr size_t SAMPLES_PER_PACKET = PKT_DATA_BYTES / 2; // 4096 complex samples (int8 Re/Im)
// complex float type (cuFFT uses cufftComplex)
using complexf = cufftComplex;
// extern "C" __global__ void kernel_pfb_sum(
//     const complexf *__restrict__ ring, // in
//     const complexf *__restrict__ taps, // pfb windows
//     complexf *__restrict__ output,     // out
//     size_t m_Nfft,
//     size_t num_taps //
// )
// {
//     size_t p = blockIdx.x * blockDim.x + threadIdx.x;
//     if (p >= m_Nfft)
//         return;

//     float acc_re = 0.0f;
//     float acc_im = 0.0f;
//     complexf acc = make_cuFloatComplex(0.0f, 0.0f);
//     for (size_t t = 0; t < num_taps; ++t)
//     {
//         size_t idx = t * m_Nfft + p;
//         complexf vin = ring[idx];
//         complexf ht = taps[t * m_Nfft + p];
//         acc_re = fmaf(vin.x, ht.x, fmaf(-vin.y, ht.y, acc_re));
//         acc_im = fmaf(vin.x, ht.y, fmaf(vin.y, ht.x, acc_im));
//     }

//     output[p].x = acc_re;
//     output[p].y = acc_im;
// }
extern "C" __global__ void stokes_IQUV_accumulate(
    complexf *__restrict__ A, //
    complexf *__restrict__ B, //
    size_t N,                 // m_Nfft
    float4 *pf4SumStokes)
{
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N)
        return;
    cuFloatComplex x = A[idx];
    cuFloatComplex y = B[idx];

    float x_re = x.x;
    float x_im = x.y;
    float y_re = y.x;
    float y_im = y.y;

    // |x|^2 和 |y|^2，用 fmaf
    float xx = fmaf(x_re, x_re, x_im * x_im);
    float yy = fmaf(y_re, y_re, y_im * y_im);

    // x * conj(y)
    // float xy_re = fmaf(x_re, y_re, x_im * y_im);  // Re(x*y*)
    // float xy_im = fmaf(x_im, y_re, -x_re * y_im); // Im(x*y*)
    // Stokes 参数
    // float I = xx + yy;
    // float Q = xx - yy;
    // float U = 2.0f * xy_re;
    // float V = 2.0f * xy_im;
    float x_rey_im = x_re * y_im;
    float x_imy_re = x_im * y_re;
    // 对应频点做原子加
    atomicAdd(&pf4SumStokes[idx].x, xx);
    atomicAdd(&pf4SumStokes[idx].y, yy);
    atomicAdd(&pf4SumStokes[idx].z, x_rey_im);
    atomicAdd(&pf4SumStokes[idx].w, x_imy_re);
}

extern "C" __global__ void kernel_pfb_sum(
    const complexf *__restrict__ ring, // 环形缓冲区 [W = num_taps*m_Nfft]
    const float *__restrict__ taps,    // 实数滤波器系数 [num_taps * m_Nfft]
    complexf *__restrict__ output,     // 输出 [m_Nfft]
    size_t m_Nfft,
    size_t num_taps,
    size_t head // 当前写指针
)
{
    size_t p = blockIdx.x * blockDim.x + threadIdx.x;
    cuFloatComplex acc = make_cuFloatComplex(0.0f, 0.0f);
    if (p >= m_Nfft)
        return;

    float acc_re = 0.0f;
    float acc_im = 0.0f;
    size_t W = num_taps * m_Nfft;

    for (size_t t = 0; t < num_taps; ++t)
    {
        size_t idx = head + t * m_Nfft + p;
        if (idx >= W)
            idx -= W; // 避免 %
        complexf vin = ring[idx];
        float ht = taps[t * m_Nfft + p];
        // acc_re = fmaf(vin.x, ht, acc_re);
        // acc_im = fmaf(vin.y, ht, acc_im);
        acc.x = fmaf(vin.x, ht, acc.x);
        acc.y = fmaf(vin.y, ht, acc.y);
    }

    output[p] = acc;
}
extern "C" __global__ void uint8_offsetIQ_to_ring(
    const uint8_t *__restrict__ src, // 移位二进制 I/Q
    complexf *__restrict__ gpuRing,  // 环形缓冲区

    size_t N,    // 复数样本数
    size_t head, // 当前写指针 (host 传入)
    size_t W     // 环形缓冲区容量 (复数样本数)
)
{
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N)
        return;

    // 读取移位二进制 I/Q
    uint8_t u_re = src[2 * idx + 1];
    uint8_t u_im = src[2 * idx];

    // 转为 signed 并归一化 [-1,1]
    float re = __int2float_rn(int(u_re) - 128) * (1.0f / 127.0f);
    float im = __int2float_rn(int(u_im) - 128) * (1.0f / 127.0f);

    complexf val;
    val.x = re;
    val.y = im;

    // 写入环形缓冲区
    size_t ring_idx = (head + idx) % W;
    gpuRing[ring_idx] = val;
}
__global__ void fftshift_1d(complexf *data, int N)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < N / 2)

    {
        complexf tmp = data[idx];
        data[idx] = data[idx + (N / 2)];
        data[idx + (N / 2)] = tmp;
    }
}
// ---------------------- Class ----------------------
class GpuPfbFft
{
public:
    GpuPfbFft(
        // moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *queueA,
        //   moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *queueB,
        int subband_id,
        // int gpu_id,
        // size_t m_Nfft,
        // size_t num_taps,
        const float *taps_host,
        float4 *result
        // int *acc_id
        )
        : // m_queueA(queueA),
          //   m_queueB(queueB),
          m_subband_id(subband_id),
          //   m_gpu_id(gpu_id),
          //   m_Nfft(m_Nfft),
          //   m_num_taps(num_taps),
          m_acc_stokes_IQUV(result)
    //   m_acc_id(acc_id)
    {
        // printf("m_Nfft: %d,taps: %d\n", m_Nfft, num_taps);

        // if ((m_Nfft % SAMPLES_PER_PACKET) != 0)
        // {
        //     throw std::invalid_argument("m_Nfft must be multiple of SAMPLES_PER_PACKET (4096)");
        // }

        auto &cfg = GlobalConfig::getInstance();
        m_config = cfg.subbands[m_subband_id];

        m_gpu_id = m_config->gpu_id;
        m_Nfft = cfg.total_nfft;
        m_queueA = &cfg.g_in_queues[m_subband_id * 2];

        m_queueB = &cfg.g_in_queues[m_subband_id * 2 + 1];
        m_packets_per_block = m_Nfft / SAMPLES_PER_PACKET;
        m_packets_per_frame = m_num_taps * m_packets_per_block;
        // printf("%d,%d",m_packets_per_block*SAMPLES_PER_PACKET,m_packets_per_frame*SAMPLES_PER_PACKET);
        // bytes to accumulate
        m_bytes_per_frame = m_packets_per_frame * PKT_DATA_BYTES;
        m_total_samples = m_num_taps * m_Nfft; // num_taps * m_Nfft samples for PFB input

        // select device
        // CUDA_CHECK(cudaSetDevice(m_gpu_id));

        // CUDA_CHECK(cudaStreamCreate(&m_stream));
        // CUDA_CHECK(cudaStreamCreate(&m_streamA));
        // CUDA_CHECK(cudaStreamCreate(&m_streamB));

        // H2D streams
        cudaStreamCreateWithFlags(&sH2DA, cudaStreamNonBlocking);
        cudaStreamCreateWithFlags(&sH2DB, cudaStreamNonBlocking);

        // 数据转换 streams
        cudaStreamCreateWithFlags(&sConvA, cudaStreamNonBlocking);
        cudaStreamCreateWithFlags(&sConvB, cudaStreamNonBlocking);

        // PFB + FFT streams
        cudaStreamCreateWithFlags(&sPFBA, cudaStreamNonBlocking);
        cudaStreamCreateWithFlags(&sPFBB, cudaStreamNonBlocking);

        // Stokes + 积分 stream
        cudaStreamCreateWithFlags(&sStokes, cudaStreamNonBlocking);

        // GPU->CPU D2H stream
        cudaStreamCreateWithFlags(&sD2H, cudaStreamNonBlocking);

        // allocate device buffers:
        CUDA_CHECK(cudaMalloc((void **)&m_rawA, m_Nfft * 2)); // rawdata formart unit8 re+imag
        CUDA_CHECK(cudaMemset(m_rawA, 0, m_Nfft * 2));
        CUDA_CHECK(cudaMalloc((void **)&m_rawB, m_Nfft * 2)); // rawdata formart unit8 re+imag
        CUDA_CHECK(cudaMemset(m_rawB, 0, m_Nfft * 2));

        size_t device_input_bytes = m_total_samples * sizeof(complexf); // after conversion int8->float complex
        m_input_ringsize = m_Nfft * m_num_taps;

        // m_input_ringbytes = m_ringcap * device_input_bytes;
        CUDA_CHECK(cudaMalloc((void **)&m_d_inputA, device_input_bytes)); // size num_taps*m_Nfft complexf
        CUDA_CHECK(cudaMemset(m_d_inputA, 0, device_input_bytes));
        CUDA_CHECK(cudaMalloc((void **)&m_d_inputB, device_input_bytes)); // size num_taps*m_Nfft complexf
        CUDA_CHECK(cudaMemset(m_d_inputB, 0, device_input_bytes));
        //  PFB
        size_t device_pfbed_bytes = m_Nfft * sizeof(complexf);
        CUDA_CHECK(cudaMalloc((void **)&m_d_pfbedA, device_pfbed_bytes)); // m_Nfft complexf
        CUDA_CHECK(cudaMemset(m_d_pfbedA, 0, device_pfbed_bytes));

        CUDA_CHECK(cudaMalloc((void **)&m_d_pfbedB, device_pfbed_bytes)); // m_Nfft complexf
        CUDA_CHECK(cudaMemset(m_d_pfbedB, 0, device_pfbed_bytes));
        // fft
        // m_fft_done.resize(m_ringcap);
        // m_ffted_bufsA.resize(m_ringcap);
        // m_ffted_bufsB.resize(m_ringcap);
        // for (int i = 0; i < m_ringcap; i++)
        {
            // m_ffted_bufsA = m_d_pfbedA;
            // m_ffted_bufsB = m_d_pfbedB;
            CUDA_CHECK(cudaMalloc((void **)&m_ffted_bufsA, device_pfbed_bytes)); // m_Nfft complexf
            CUDA_CHECK(cudaMemset(m_ffted_bufsA, 0, device_pfbed_bytes));
            CUDA_CHECK(cudaMalloc((void **)&m_ffted_bufsB, device_pfbed_bytes)); // m_Nfft complexf
            CUDA_CHECK(cudaMemset(m_ffted_bufsB, 0, device_pfbed_bytes));

            // cudaEventCreateWithFlags(&m_fft_done[i], cudaEventDisableTiming);
        }
        // stockes and ACC
        // CUDA_CHECK(cudaMalloc((void **)&m_acc_stokes_IQUV, m_Nfft * sizeof(float4))); //
        // CUDA_CHECK(cudaMemset(m_acc_stokes_IQUV, 0, m_Nfft * sizeof(float4)));
        // final result
        CUDA_CHECK(cudaMallocHost((void **)&m_result_ptr, m_Nfft * sizeof(float4)));
        cudaMemset(m_result_ptr, 0, m_Nfft * sizeof(float4));

        // allocate taps on device and copy
        size_t taps_bytes = m_total_samples * sizeof(float);
        CUDA_CHECK(cudaMalloc((void **)&m_filters, taps_bytes));
        if (taps_host)
        {
            CUDA_CHECK(cudaMemcpy(m_filters, taps_host, taps_bytes, cudaMemcpyHostToDevice));
        }
        else
        {
            // zero taps if not provided
            CUDA_CHECK(cudaMemset(m_filters, 0, taps_bytes));
        }

        // create cuFFT plan for m_Nfft complex->complex
        CUFFT_CHECK(cufftPlan1d(&m_planA, static_cast<int>(m_Nfft), CUFFT_C2C, 1));
        CUFFT_CHECK(cufftSetStream(m_planA, sPFBA));
        CUFFT_CHECK(cufftPlan1d(&m_planB, static_cast<int>(m_Nfft), CUFFT_C2C, 1));
        CUFFT_CHECK(cufftSetStream(m_planB, sPFBB));
        // compute kernel launch sizes
        m_blk_convert = 256;
        m_grd_total = (m_total_samples + m_blk_convert - 1) / m_blk_convert;

        m_grd_nfft = (m_Nfft + m_blk_convert - 1) / m_blk_convert;
        // cudaEventCreate(&eventA);
        // cudaEventCreate(&eventB);
        // cudaEventCreate(&event_finish);
        cudaEventCreate(&evtH2DA_done);
        cudaEventCreate(&evtH2DB_done);

        cudaEventCreate(&evtConvA_done);
        cudaEventCreate(&evtConvB_done);

        cudaEventCreate(&evtPFBA_done);
        cudaEventCreate(&evtPFBB_done);

        cudaEventCreate(&evtStokes_done);

        // if (!dadaFile.open("/data/example.dada"))
        // {
        //     std::cerr << "Failed to open file" << std::endl;
        // }
        m_acc_len = cfg.integration_time() / cfg.fft_period();
        // printf("%f,%f\n",cfg.integration_time(),cfg.fft_period());
        // printf("acc_len = %d\n",m_acc_len);
        // std::string fname = "subband_" + std::to_string(m_subband_id);
        // std::thread writer_thread([fname, this]()
        //                           {
        //                               while (1)
        //                               {
        //                                   cudaEventSynchronize(event_finish); // 等待 GPU 完成
        //                                   write_to_file(fname, m_result_ptr, m_Nfft);
        //                               } });
        // writer_thread.detach();

        SLOT_SIZE = m_Nfft * sizeof(float4);

        m_hring.resize(NUM_SLOTS);
        m_hevents.resize(NUM_SLOTS);
        m_hused.resize(NUM_SLOTS, false);
        m_timestamp.resize(NUM_SLOTS);

        for (int i = 0; i < NUM_SLOTS; i++)
        {
            cudaMallocHost((void **)&m_hring[i], SLOT_SIZE); // pinned memory
            memset(m_hring[i], 0, SLOT_SIZE);
            cudaEventCreateWithFlags(&m_hevents[i], cudaEventBlockingSync);
        }
        std::thread writer([this]()
                           {
                               send_data();
                               //    write_to_file();
                           });
        writer.detach();
    }
    // void select_windows(int idx)
    // {
    //     auto windows = m_config[m_subband_id].windows;
    //     auto total_BW = m_config[m_subband_id].BW;

    // }
    bool set_static_arp(const std::string& ip,
                        const std::string& mac,
                        const std::string& dev)
    {
        std::string cmd =
            "ip neigh replace " + ip +
            " lladdr " + mac +
            " dev " + dev +
            " nud permanent";

        int ret = system(cmd.c_str());

        return (ret == 0);
    }
    void send_data()
    {
        int idx = 0;

        auto &cfg = GlobalConfig::getInstance();
        int channels = cfg.win_channels;
        if (set_static_arp(cfg.Storage_node_ip,
                        cfg.Storage_node_mac,
                        cfg.Sender_Nic
                        ))
        {
            cfg.logger_->info("Static ARP set success\n");
        }
        else
        {
            cfg.logger_->error("Static ARP set failed\n");
        }
        while (1)
        {
            if (cudaEventQuery(m_hevents[idx]) == cudaSuccess && m_hused[idx])
            {
                float max = 0;
                int max_freq = 0;
                for (int i = 0; i < m_Nfft; i++)
                {
                    if (m_hring[idx][i].x > max)
                    {
                        max = m_hring[idx][i].x;
                        max_freq = i;
                    }
                }
                if(cfg.Debug_mode)
                    printf("%.2f Mhz+(%d)+%f Mhz\n", m_config->start_freq * 1e-6, max_freq, (float)max_freq * 256 / (float)m_Nfft);

                for (int i = 0; i < m_config->windows.size(); i++)
                {
                    size_t start_idx = m_config->windows[i]->start_idx;

                    m_config->windows[i]->header.timestamp_ns = m_timestamp[idx];
                    m_config->windows[i]->sender.send_spectrum(m_config->windows[i]->header, &m_hring[idx][start_idx], channels * sizeof(float4));
                }
                m_hused[idx] = false;
                idx = (idx + 1) % NUM_SLOTS;
            }
        }
    }
    void write_to_file()
    {

        std::string filename = "/data/subband_" + std::to_string(m_subband_id);
        int idx = 0;
        while (1)
        {
            // cudaEventSynchronize(m_hevents[idx]);
            if (cudaEventQuery(m_hevents[idx]) == cudaSuccess && m_hused[idx])
            {

                if (!dadaFile.open(filename))
                {
                    std::cerr << "Failed to open file" << std::endl;
                }
                dadaFile.writeData(m_hring[idx], m_Nfft * sizeof(float4));
                dadaFile.close();

                // float max = 0;
                // int max_freq = 0;
                // for (int i = 0; i < m_Nfft; i++)
                // {
                //     if (m_hring[idx][i].x > max)
                //     {
                //         max = m_hring[idx][i].x;
                //         max_freq = i;
                //     }
                // }
                // printf("%f Mhz\n", (float)max_freq / m_Nfft * 256);

                m_hused[idx] = false;
                idx = (idx + 1) % NUM_SLOTS;
            }
            // else
            // {
            //     std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // }
        }

        // // 设置 header
        // dadaFile.setHeader("OBS_FREQ", "1400.0");
        // dadaFile.setHeader("NCHAN", "1024");
        // dadaFile.setHeader("TSAMP", "64e-6");

        // // 写 header
        // dadaFile.writeHeader();
    }
    ~GpuPfbFft()
    {
    }
    // 静态函数，作为回调入口
    static void CUDART_CB write_callback(void *userData)
    {
        auto *self = reinterpret_cast<GpuPfbFft *>(userData);
        self->write_to_file2();
    }

    void write_to_file2()
    {
        std::string fname = "subband_" + std::to_string(m_subband_id);
        if (!dadaFile.open(fname))
        {
            std::cerr << "Failed to open file" << std::endl;
        }
        dadaFile.writeData(m_hring[m_hhead], m_Nfft * sizeof(float4));
        dadaFile.close();
        float max = 0;
        int max_freq = 0;
        for (int i = 0; i < m_Nfft; i++)
        {
            if (m_hring[m_hhead][i].x > max)
            {
                max = m_hring[m_hhead][i].x;
                max_freq = i;
            }
        }
        // printf("%f Mhz  \n", (float)max_freq / m_Nfft * 256);
        // std::ofstream ofs(m_fname, std::ios::binary);
        // ofs.write(reinterpret_cast<char *>(m_h_result), m_Nfft * sizeof(float4));
        // ofs.close();
        // std::cout << "Write complete: " << m_fname << std::endl;
    }
    uint32_t expected_frame_number;
    uint64_t last_second;
    FILE *logfile = nullptr;

    bool accumulate_one_block()
    {
        auto &cfg = GlobalConfig::getInstance();
        // void *testdata;
        // cudaMallocHost((void **)&testdata, cfg.batchsize() * sizeof(Packet));
        // memset(testdata, 0, cfg.batchsize() * sizeof(Packet));
        // cudaMemcpyAsync(m_rawA, testdata, cfg.batchsize() * sizeof(Packet), cudaMemcpyHostToDevice, m_stream);
        // cudaMemcpyAsync(m_rawB, testdata, cfg.batchsize() * sizeof(Packet), cudaMemcpyHostToDevice, m_stream);
        // Cudamemf(testdata);
        // return false;

        // cudaEvent_t event;
        // cudaEventCreateWithFlags(&event, cudaEventDisableTiming);
        PacketBatch *readblockA = nullptr;
        PacketBatch *readblockB = nullptr;

        // auto futA = std::async(std::launch::async, [&]
        //                        { m_queueA->wait_dequeue(readblockA); });
        // auto futB = std::async(std::launch::async, [&]
        //                        { m_queueB->wait_dequeue(readblockB); });

        // // 等待两个任务完成
        // futA.get();
        // futB.get();
        m_queueA->wait_dequeue(readblockA);
        // CUDA_CHECK(cudaStreamSynchronize(sH2DA));
        m_queueB->wait_dequeue(readblockB);
        // CUDA_CHECK(cudaStreamSynchronize(sH2DB));
        if (m_queueA->size_approx() > cfg.QUEUE_CAPACITY * 0.95)
            cfg.logger_->debug("Buffed {} batch in queue", m_queueA->size_approx());
        // printf("Got dual block data on sub band %d", m_subband_id);
        // TODO GOT PKT ID FROM PKT
        
        size_t m_pktidA = readblockA->pkt_id[0];
        size_t m_pktidB = readblockB->pkt_id[0];
        if (m_pktidA != m_pktidB)
            cfg.logger_->warn("Subband {} pktid A != B. {} !={} ", m_subband_id, m_pktidA, m_pktidB);
        m_timestamp[m_hhead] = readblockA->timestamps[0];

        cudaMemcpyAsync(m_rawA, readblockA->buffer, cfg.batchsize() * sizeof(Packet), cudaMemcpyHostToDevice, sH2DA);
        cudaEventRecord(evtH2DA_done, sH2DA);
        cudaMemcpyAsync(m_rawB, readblockB->buffer, cfg.batchsize() * sizeof(Packet), cudaMemcpyHostToDevice, sH2DB);
        cudaEventRecord(evtH2DB_done, sH2DB);
        // TODO check A AND B sync state

        // printf("got one block data on gpu");
        return true;
    }

    void submit_PFB_FFT()
    {

        // trans raw int8 block data to float and save to gpu ring
        int blockSize = 256; // 推荐 128 / 256 / 512
        int gridSize = (m_Nfft + blockSize - 1) / blockSize;

        cudaStreamWaitEvent(sConvA, evtH2DA_done, 0);
        uint8_offsetIQ_to_ring<<<gridSize, blockSize, 0, sConvA>>>(m_rawA, m_d_inputA, m_Nfft, m_in_head, m_input_ringsize);
        cudaEventRecord(evtConvA_done, sConvA);
        uint8_offsetIQ_to_ring<<<gridSize, blockSize, 0, sConvB>>>(m_rawB, m_d_inputB, m_Nfft, m_in_head, m_input_ringsize);
        cudaEventRecord(evtConvB_done, sConvB);
        m_in_head = (m_in_head + m_Nfft) % m_input_ringsize;
        // DO PFB
        cudaStreamWaitEvent(sPFBA, evtConvA_done, 0);
        kernel_pfb_sum<<<gridSize, blockSize, 0, sPFBA>>>(
            m_d_inputA, m_filters, m_d_pfbedA, m_Nfft, m_num_taps, m_in_head);
        cudaStreamWaitEvent(sPFBB, evtConvB_done, 0);
        kernel_pfb_sum<<<gridSize, blockSize, 0, sPFBB>>>(
            m_d_inputB, m_filters, m_d_pfbedB, m_Nfft, m_num_taps, m_in_head);
        // DO FFT
        // m_fft_ring_id = m_block_id % m_ringcap;

        cufftExecC2C(m_planA, m_d_pfbedA, m_ffted_bufsA, CUFFT_FORWARD);
        fftshift_1d<<<gridSize, blockSize, 0, sPFBA>>>(
            m_ffted_bufsA, m_Nfft);
        cudaEventRecord(evtPFBA_done, sPFBA);
        cufftExecC2C(m_planB, m_d_pfbedB, m_ffted_bufsB, CUFFT_FORWARD);
        fftshift_1d<<<gridSize, blockSize, 0, sPFBB>>>(
            m_ffted_bufsB, m_Nfft);
        cudaEventRecord(evtPFBB_done, sPFBB);

        // CUDA_CHECK(cudaStreamSynchronize(m_stream));
        // CUDA_CHECK(cudaStreamSynchronize(m_stream));
        m_block_id++;
    }

    // block until stream completes this object's work
    // void synchronize()
    // {
    //     CUDA_CHECK(cudaStreamSynchronize(m_stream));
    //     CUDA_CHECK(cudaStreamSynchronize(m_stream));
    // }

    void StokesAcc()
    {
        // int idx = head % NUM_SLOTS;
        int blockSize = 256; //
        int gridSize = (m_Nfft + blockSize - 1) / blockSize;
        // if (m_acc_id == 0)
        //     CUDA_CHECK(cudaMemsetAsync(m_acc_stokes_IQUV, 0, m_Nfft * sizeof(float4), m_stream));
        // DO stockes and Acc
        cudaStreamWaitEvent(sStokes, evtPFBA_done, 0);
        cudaStreamWaitEvent(sStokes, evtPFBB_done, 0);
        stokes_IQUV_accumulate<<<gridSize, blockSize, 0, sStokes>>>(
            m_ffted_bufsA, m_ffted_bufsB, m_Nfft, m_acc_stokes_IQUV);

        (m_acc_id)++;
        if (m_acc_id == m_acc_len)
        {
            cudaEventRecord(evtStokes_done, sStokes);
            // TODO gpu ring output
            cudaStreamWaitEvent(sD2H, evtStokes_done, 0);
            m_acc_id = 0;
            cudaMemcpyAsync(m_hring[m_hhead], m_acc_stokes_IQUV, m_Nfft * sizeof(float4), cudaMemcpyDeviceToHost, sD2H);
            // cudaLaunchHostFunc(sD2H, GpuPfbFft::write_callback, this);

            cudaEventRecord(m_hevents[m_hhead], sD2H);
            m_hused[m_hhead] = true;
            CUDA_CHECK(cudaMemsetAsync(m_acc_stokes_IQUV, 0, m_Nfft * sizeof(float4), sD2H));
            m_hhead = (m_hhead + 1) % NUM_SLOTS;
        }

        // CUDA_CHECK(cudaStreamSynchronize(m_stream));
    }

private:
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueA; // 队列
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueB;
    int m_subband_id;
    int m_gpu_id = 0;
    size_t m_Nfft = 0;
    size_t m_num_taps = 4;
    size_t m_packets_per_block = 0;
    size_t m_packets_per_frame = 0;
    size_t m_bytes_per_frame = 0;
    size_t m_total_samples = 0; // num_taps * m_Nfft

    float4 *m_result_ptr = nullptr;
    // device buffers
    uint8_t *m_rawA = nullptr;
    uint8_t *m_rawB = nullptr;
    // input
    complexf *m_d_inputA = nullptr; // num_taps*m_Nfft complexf (after int8->float conversion)
    complexf *m_d_inputB = nullptr;
    // int m_ringcap = 4;
    // int m_input_ringbytes = 0; // input ring bytes
    int m_input_ringsize = 0;
    int m_in_head = 0;
    // PFB
    float *m_filters = nullptr;     // num_taps*m_Nfft complexf
    complexf *m_d_pfbedA = nullptr; // m_Nfft complexf (PFB output then FFT in-place)
    complexf *m_d_pfbedB = nullptr;
    // fft
    complexf *m_ffted = nullptr;
    // cuFFT plan
    cufftHandle m_planA = 0;
    cufftHandle m_planB = 0;
    // CUDA stream
    // cudaStream_t m_stream = nullptr;
    // cudaStream_t m_streamA = nullptr;
    // cudaStream_t m_streamB = nullptr;
    // cudaEvent_t eventA, eventB, event_finish;

    cudaStream_t sH2DA, sH2DB;
    cudaStream_t sConvA, sConvB;
    cudaStream_t sPFBA, sPFBB;
    cudaStream_t sStokes;
    cudaStream_t sD2H;

    cudaEvent_t evtH2DA_done, evtH2DB_done;
    cudaEvent_t evtConvA_done, evtConvB_done;
    cudaEvent_t evtPFBA_done, evtPFBB_done;
    cudaEvent_t evtStokes_done;

    // FFT Result
    std::vector<cudaEvent_t> m_fft_done;
    cufftComplex *m_ffted_bufsA;
    cufftComplex *m_ffted_bufsB;

    int m_block_id = 0;
    // int m_fft_ring_id = 0;
    // stockes and ACC
    float4 *m_acc_stokes_IQUV;
    int m_acc_len;
    int m_acc_id = 0;

    int m_resframe_id = 0;
    // kernel launch parameters
    int m_blk_convert = 256;
    int m_grd_total = 0;
    int m_grd_nfft = 0;
    // signal process
    int sr = 256e6;             // sampling rate
    float accumulated_time = 1; // second

    // time and sync
    uint64_t m_seconds = 0, m_pre_seconds = 0;
    uint64_t m_frame_number = 0;
    uint64_t global_packet_id = 0;
    uint64_t fps = 62500;
    std::ofstream fout;
    Writer dadaFile;
    const int NUM_SLOTS = 16;
    size_t SLOT_SIZE = 0; // 每槽字节数
    std::vector<float4 *> m_hring;
    std::vector<cudaEvent_t> m_hevents; // 每槽传输完成事件
    std::vector<bool> m_hused;
    std::vector<uint64_t> m_timestamp;
    int m_hhead = 0; // GPU 写入位置
    SubbandConfig *m_config;
};
