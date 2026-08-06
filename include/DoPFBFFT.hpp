#pragma once
#include <cstdint>
#include <cstring>
#include <cuda_runtime.h>
#include <cufft.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
// #include <syncstream>
#include <inttypes.h>
// #include <VDIFReader.hpp>
#include <Globalcfg.hpp>
#include <Writer.h>
#include <cuda_fp16.h>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

#define CUDA_CHECK(call)                                                                                                                                       \
  do {                                                                                                                                                         \
    cudaError_t _e = (call);                                                                                                                                   \
    if (_e != cudaSuccess) {                                                                                                                                   \
      std::cerr << "CUDA error " << cudaGetErrorString(_e) << " at " << __FILE__ << ":" << __LINE__ << std::endl;                                              \
      throw std::runtime_error(cudaGetErrorString(_e));                                                                                                        \
    }                                                                                                                                                          \
  } while (0)

#define CUFFT_CHECK(call)                                                                                                                                      \
  do {                                                                                                                                                         \
    cufftResult _r = (call);                                                                                                                                   \
    if (_r != CUFFT_SUCCESS) {                                                                                                                                 \
      std::cerr << "cuFFT error " << _r << " at " << __FILE__ << ":" << __LINE__ << std::endl;                                                                 \
      throw std::runtime_error("cuFFT error");                                                                                                                 \
    }                                                                                                                                                          \
  } while (0)

// each VDIF packet bytes and samples
static constexpr size_t PKT_DATA_BYTES = 8192;
static constexpr size_t SAMPLES_PER_PACKET = PKT_DATA_BYTES / 2; // 4096 complex samples (int8 Re/Im)
// complex float type (cuFFT uses cufftComplex)
using complexf = cufftComplex;
extern "C" __global__ void stokes_IQUV_accumulate(complexf *__restrict__ A, //
                                                  complexf *__restrict__ B, //
                                                  size_t N,                 // m_Nfft
                                                  float4 *pf4SumStokes) {
  size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= N) return;
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

extern "C" __global__ void kernel_pfb_sum(const complexf *__restrict__ ring, // 环形缓冲区 [W = num_taps*m_Nfft]
                                          const float *__restrict__ taps,    // 实数滤波器系数 [num_taps * m_Nfft]
                                          complexf *__restrict__ output,     // 输出 [m_Nfft]
                                          size_t m_Nfft, size_t num_taps,
                                          size_t head // 当前写指针
) {
  size_t p = blockIdx.x * blockDim.x + threadIdx.x;
  cuFloatComplex acc = make_cuFloatComplex(0.0f, 0.0f);
  if (p >= m_Nfft) return;
  
  size_t W = num_taps * m_Nfft;

  for (size_t t = 0; t < num_taps; ++t) {
    size_t idx = head + t * m_Nfft + p;
    if (idx >= W) idx -= W; // 避免 %
    complexf vin = ring[idx];
    float ht = taps[t * m_Nfft + p];
    acc.x = fmaf(vin.x, ht, acc.x);
    acc.y = fmaf(vin.y, ht, acc.y);
  }

  output[p] = acc;
}
constexpr float scale = 1.0f / 127.0f;
extern "C" __global__ void uint8_offsetIQ_to_ring(const uint8_t *__restrict__ src, // 移位二进制 I/Q
                                                  complexf *__restrict__ gpuRing,  // 环形缓冲区

                                                  size_t N,    // 复数样本数
                                                  size_t head, // 当前写指针 (host 传入)
                                                  size_t W     // 环形缓冲区容量 (复数样本数)
) {
  size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= N) return;

  const uchar2 *iq = reinterpret_cast<const uchar2 *>(src);
  uchar2 s = iq[idx];
  float re = __int2float_rn((int)s.y - 128) * scale;
  float im = __int2float_rn((int)s.x - 128) * scale;
  // 写入缓冲区
  size_t ring_idx = (head + idx) % W;
  gpuRing[ring_idx] = make_float2(re, im);
}
__global__ void fftshift_1d(complexf *data, int N) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx < N / 2)

  {
    complexf tmp = data[idx];
    data[idx] = data[idx + (N / 2)];
    data[idx + (N / 2)] = tmp;
  }
}
enum class NoiseState : uint8_t { OFF = 0, ON = 1, BLANK = 2 };

struct HostRingSlot {
  float4 *data = nullptr;

  cudaEvent_t event = nullptr;

  bool used = false;

  uint64_t timestamp = 0;
  NoiseState noise_state = NoiseState::OFF;
  uint32_t cycle_id = 0;
};
// ---------------------- Class ----------------------
class GpuPfbFft {
public:
  GpuPfbFft(int subband_id, const float *taps_host) : m_subband_id(subband_id) {
    auto &cfg = GlobalConfig::getInstance();
    m_config = cfg.subbands[m_subband_id];

    m_gpu_id = m_config->gpu_id;
    m_Nfft = cfg.total_nfft;
    m_queueA = &cfg.streams[m_subband_id * 2].queue;
    m_queueB = &cfg.streams[m_subband_id * 2 + 1].queue;

    // bytes to accumulate
    m_bytes_per_frame = m_packets_per_frame * PKT_DATA_BYTES;
    m_total_samples = m_num_taps * m_Nfft; // num_taps * m_Nfft samples for PFB input
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
    cudaStreamCreateWithFlags(&sD2H, cudaStreamNonBlocking);

    // allocate device buffers:
    CUDA_CHECK(cudaMalloc((void **)&m_rawA, m_Nfft * 2)); // rawdata formart unit8 re+imag
    CUDA_CHECK(cudaMemset(m_rawA, 0, m_Nfft * 2));
    CUDA_CHECK(cudaMalloc((void **)&m_rawB, m_Nfft * 2)); // rawdata formart unit8 re+imag
    CUDA_CHECK(cudaMemset(m_rawB, 0, m_Nfft * 2));

    size_t device_input_bytes = m_total_samples * sizeof(complexf); // after conversion int8->float complex
    m_pfb_ringsize = m_Nfft * m_num_taps;

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

    CUDA_CHECK(cudaMalloc((void **)&m_ffted_bufsA, device_pfbed_bytes)); // m_Nfft complexf
    CUDA_CHECK(cudaMemset(m_ffted_bufsA, 0, device_pfbed_bytes));
    CUDA_CHECK(cudaMalloc((void **)&m_ffted_bufsB, device_pfbed_bytes)); // m_Nfft complexf
    CUDA_CHECK(cudaMemset(m_ffted_bufsB, 0, device_pfbed_bytes));

    CUDA_CHECK(cudaMalloc((void **)&m_acc_stokes_ON, m_Nfft * sizeof(float4)));

    CUDA_CHECK(cudaMalloc((void **)&m_acc_stokes_OFF, m_Nfft * sizeof(float4)));

    CUDA_CHECK(cudaMemset(m_acc_stokes_ON, 0, m_Nfft * sizeof(float4)));

    CUDA_CHECK(cudaMemset(m_acc_stokes_OFF, 0, m_Nfft * sizeof(float4)));

    // allocate taps on device and copy
    size_t taps_bytes = m_total_samples * sizeof(float);
    CUDA_CHECK(cudaMalloc((void **)&m_filters, taps_bytes));
    if (taps_host) {
      CUDA_CHECK(cudaMemcpy(m_filters, taps_host, taps_bytes, cudaMemcpyHostToDevice));
    } else {
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

    cal_blank_len = static_cast<uint32_t>(std::ceil(10e-3 / cfg.fft_period()));

    SLOT_SIZE = m_Nfft * sizeof(float4);

    m_hring.resize(NUM_SLOTS);

    for (auto &slot : m_hring) {
      cudaMallocHost(reinterpret_cast<void **>(&slot.data), SLOT_SIZE);

      memset(slot.data, 0, SLOT_SIZE);

      cudaEventCreateWithFlags(&slot.event, cudaEventBlockingSync);

      slot.used = false;
      slot.timestamp = 0;
      slot.noise_state = NoiseState::OFF;
      slot.cycle_id = 0;
    }
    std::thread pushresult([this]() {
      send_data();
      //    write_to_file();
    });
    pushresult.detach();;
  }
  // Avoid lose arp while start dpdk
  bool set_static_arp(const std::string &ip, const std::string &mac, const std::string &dev) {
    std::string cmd = "ip neigh replace " + ip + " lladdr " + mac + " dev " + dev + " nud permanent";

    int ret = system(cmd.c_str());

    return (ret == 0);
  }
  void send_data() {
    int idx = 0;

    auto &cfg = GlobalConfig::getInstance();
    int channels = cfg.win_channels;
    if (set_static_arp(cfg.Storage_node_ip, cfg.Storage_node_mac, cfg.Sender_Nic)) {
      cfg.logger_->info("Static ARP set success\n");
    } else {
      cfg.logger_->error("Static ARP set failed\n");
    }

    while (1) {
      auto &slot = m_hring[idx];

      if (cudaEventQuery(slot.event) == cudaSuccess && slot.used) {

        if (cfg.Debug_mode) {
          float max = 0;
          int max_freq = 0;
          for (size_t i = 0; i < m_Nfft; i++) {
            // if (m_hring[idx][i].x > max)
            if (slot.data[i].x > max) {
              max = m_hring[idx].data[i].x;
              max_freq = i;
            }
          }
          printf("%.2f Mhz+(%d)+%f Mhz\n", m_config->start_freq * 1e-6, max_freq, (float)max_freq * 256 / (float)m_Nfft);
        }

        // std::cout << acc_noise_state[idx] << std::endl;
        for (size_t i = 0; i < m_config->windows.size(); i++) {
          size_t start_idx = m_config->windows[i]->start_idx;
          m_config->windows[i]->header.timestamp_ns = slot.timestamp;

          m_config->windows[i]->header.noise_state = static_cast<uint32_t>(slot.noise_state);

          m_config->windows[i]->sender.send_spectrum(m_config->windows[i]->header, &slot.data[start_idx], channels * sizeof(float4));
        }
        slot.used = false;
        idx = (idx + 1) % NUM_SLOTS;
      }
    }
  }
  ~GpuPfbFft() {}
  uint32_t expected_frame_number;
  uint64_t last_second;
  FILE *logfile = nullptr;

  bool accumulate_one_block() {
    auto &cfg = GlobalConfig::getInstance();
    PacketBatch *readblockA = nullptr;
    PacketBatch *readblockB = nullptr;

    // futB.get();
    m_queueA->wait_dequeue(readblockA);
    m_queueB->wait_dequeue(readblockB);
    if (m_queueA->size_approx() > cfg.QUEUE_CAPACITY * 0.95) cfg.logger_->debug("Buffed {} batch in queue,more than 95%%", m_queueA->size_approx());
    //  printf("Got dual block data on sub band %d", m_subband_id);
    //
    size_t m_pktidA = readblockA->pkt_id[0];
    size_t m_pktidB = readblockB->pkt_id[0];
    if (cfg.Debug_mode) {
      if (m_pktidA != m_pktidB) cfg.logger_->warn("Subband {} pktid A != B. {} !={} ", m_subband_id, m_pktidA, m_pktidB);
    }
    for (int i = 0; i < cfg.batchsize(); i++) {
      if (readblockA->pkt_id[i] != m_pktidA + i) {
        readblockA->valid = false;
      }
      if (readblockB->pkt_id[i] != m_pktidB + i) {
        readblockB->valid = false;
      }
    }
    if (readblockA->valid == false or readblockB->valid == false) {
      memset(readblockA->buffer, 0, m_Nfft * 2);
      memset(readblockB->buffer, 0, m_Nfft * 2);
    }
    m_noise_state = static_cast<NoiseState>(readblockA->noise_state[0]);
    cudaMemcpyAsync(m_rawA, readblockA->buffer, m_Nfft * 2, cudaMemcpyHostToDevice, sH2DA);
    cudaEventRecord(evtH2DA_done, sH2DA);
    cudaMemcpyAsync(m_rawB, readblockB->buffer, m_Nfft * 2, cudaMemcpyHostToDevice, sH2DB);
    cudaEventRecord(evtH2DB_done, sH2DB);
    // TODO check A AND B sync state

    // printf("got one block data on gpu");
    return true;
  }
  void submit_PFB_FFT() {
    // trans raw int8 block data to float and save to gpu ring
    int blockSize = 256; // 推荐 128 / 256 / 512
    int gridSize = (m_Nfft + blockSize - 1) / blockSize;

    cudaStreamWaitEvent(sConvA, evtH2DA_done, 0);
    // input: m_rawA, m_rawB output: m_d_inputA, m_d_inputB
    uint8_offsetIQ_to_ring<<<gridSize, blockSize, 0, sConvA>>>(m_rawA, m_d_inputA, m_Nfft, m_pfb_head, m_pfb_ringsize);
    cudaEventRecord(evtConvA_done, sConvA);
    uint8_offsetIQ_to_ring<<<gridSize, blockSize, 0, sConvB>>>(m_rawB, m_d_inputB, m_Nfft, m_pfb_head, m_pfb_ringsize);
    cudaEventRecord(evtConvB_done, sConvB);
    m_pfb_head = (m_pfb_head + m_Nfft) % m_pfb_ringsize;
    // DO PFB
    cudaStreamWaitEvent(sPFBA, evtConvA_done, 0);
    kernel_pfb_sum<<<gridSize, blockSize, 0, sPFBA>>>(m_d_inputA, m_filters, m_d_pfbedA, m_Nfft, m_num_taps, m_pfb_head);
    cudaStreamWaitEvent(sPFBB, evtConvB_done, 0);
    kernel_pfb_sum<<<gridSize, blockSize, 0, sPFBB>>>(m_d_inputB, m_filters, m_d_pfbedB, m_Nfft, m_num_taps, m_pfb_head);
    // DO FFT
    // m_fft_ring_id = m_block_id % m_ringcap;

    cufftExecC2C(m_planA, m_d_pfbedA, m_ffted_bufsA, CUFFT_FORWARD);
    fftshift_1d<<<gridSize, blockSize, 0, sPFBA>>>(m_ffted_bufsA, m_Nfft);
    cudaEventRecord(evtPFBA_done, sPFBA);
    cufftExecC2C(m_planB, m_d_pfbedB, m_ffted_bufsB, CUFFT_FORWARD);
    fftshift_1d<<<gridSize, blockSize, 0, sPFBB>>>(m_ffted_bufsB, m_Nfft);
    cudaEventRecord(evtPFBB_done, sPFBB);
    // m_block_id++;
  }

  void StokesAcc() {
    auto &cfg = GlobalConfig::getInstance();

    int blockSize = 256;
    int gridSize = (m_Nfft + blockSize - 1) / blockSize;

    cudaStreamWaitEvent(sD2H, evtPFBA_done, 0);

    cudaStreamWaitEvent(sD2H, evtPFBB_done, 0);
    if (cfg.cal_mode) {

      /*
       * ON积分
       */
      if (m_noise_state == NoiseState::ON) {
        m_acc_on_id++;
        if (m_acc_on_id >= cal_blank_len && m_acc_on_id < m_acc_len - cal_blank_len) {

          stokes_IQUV_accumulate<<<gridSize, blockSize, 0, sD2H>>>(m_ffted_bufsA, m_ffted_bufsB, m_Nfft, m_acc_stokes_ON);
        }
        /*
         * ON累计时间达到积分时间
         */
        if (m_acc_on_id >= m_acc_len) {

          if (m_hring[m_hhead].used) {
            cfg.logger_->warn("GPU ring buffer overflow slot {}", m_hhead);
          }

          cudaMemcpyAsync(m_hring[m_hhead].data, m_acc_stokes_ON, m_Nfft * sizeof(float4), cudaMemcpyDeviceToHost, sD2H);

          m_hring[m_hhead].noise_state = NoiseState::ON;

          cudaEventRecord(m_hring[m_hhead].event, sD2H);

          m_hring[m_hhead].used = true;

          cudaMemsetAsync(m_acc_stokes_ON, 0, m_Nfft * sizeof(float4), sD2H);

          m_acc_on_id = 0;

          m_hhead = (m_hhead + 1) % NUM_SLOTS;
        }
      }

      /*
       * OFF积分
       */
      else {

        if (m_acc_off_id >= cal_blank_len && m_acc_off_id < m_acc_len - cal_blank_len) {

          stokes_IQUV_accumulate<<<gridSize, blockSize, 0, sD2H>>>(m_ffted_bufsA, m_ffted_bufsB, m_Nfft, m_acc_stokes_OFF);
        }
        /*
         * OFF累计时间达到积分时间
         */
        if (m_acc_off_id >= m_acc_len) {

          if (m_hring[m_hhead].used) {
            cfg.logger_->warn("GPU ring buffer overflow slot {}", m_hhead);
          }

          cudaMemcpyAsync(m_hring[m_hhead].data, m_acc_stokes_OFF, m_Nfft * sizeof(float4), cudaMemcpyDeviceToHost, sD2H);

          m_hring[m_hhead].noise_state = NoiseState::OFF;

          cudaEventRecord(m_hring[m_hhead].event, sD2H);

          m_hring[m_hhead].used = true;

          cudaMemsetAsync(m_acc_stokes_OFF, 0, m_Nfft * sizeof(float4), sD2H);

          m_acc_off_id = 0;

          m_hhead = (m_hhead + 1) % NUM_SLOTS;
        }
      }
    } else {
      stokes_IQUV_accumulate<<<gridSize, blockSize, 0, sD2H>>>(m_ffted_bufsA, m_ffted_bufsB, m_Nfft, m_acc_stokes_OFF);
      if (m_acc_on_id >= m_acc_len) {

        if (m_hring[m_hhead].used) {
          cfg.logger_->warn("GPU ring buffer overflow slot {}", m_hhead);
        }

        cudaMemcpyAsync(m_hring[m_hhead].data, m_acc_stokes_OFF, m_Nfft * sizeof(float4), cudaMemcpyDeviceToHost, sD2H);

        m_hring[m_hhead].noise_state = NoiseState::OFF;

        cudaEventRecord(m_hring[m_hhead].event, sD2H);

        m_hring[m_hhead].used = true;

        cudaMemsetAsync(m_acc_stokes_OFF, 0, m_Nfft * sizeof(float4), sD2H);

        m_acc_on_id = 0;

        m_hhead = (m_hhead + 1) % NUM_SLOTS;
      }
    }
  }

private:
  moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueA; // 队列
  moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queueB;
  int m_subband_id;
  int m_gpu_id = 0;
  size_t m_Nfft = 0;
  size_t m_num_taps = 4;
  size_t m_packets_per_frame = 0;
  size_t m_bytes_per_frame = 0;
  size_t m_total_samples = 0; // num_taps * m_Nfft

  // device buffers
  uint8_t *m_rawA = nullptr;
  uint8_t *m_rawB = nullptr;
  // input
  complexf *m_d_inputA = nullptr; // num_taps*m_Nfft complexf (after int8->float conversion)
  complexf *m_d_inputB = nullptr;
  size_t m_pfb_ringsize = 0;
  int m_pfb_head = 0; // head points to the oldest sample of current PFB window
  // PFB
  float *m_filters = nullptr;     // num_taps*m_Nfft complexf, point to pbf window coefficients
  complexf *m_d_pfbedA = nullptr; // m_Nfft complexf (PFB output then FFT in-place)
  complexf *m_d_pfbedB = nullptr;
  // fft
  complexf *m_ffted = nullptr;
  // cuFFT plan
  cufftHandle m_planA = 0;
  cufftHandle m_planB = 0;
  // CUDA streams

  cudaStream_t sH2DA, sH2DB;
  cudaStream_t sConvA, sConvB;
  cudaStream_t sPFBA, sPFBB;

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

  // stockes and ACC
  float4 *m_acc_stokes_ON;

  float4 *m_acc_stokes_OFF;
  u_int m_acc_len;
  u_int m_acc_id = 0;
  bool cal_mode_unstable = false;
  uint32_t cal_blank_len = 0;
  int m_resframe_id = 0;
  // kernel launch parameters
  int m_blk_convert = 256;
  int m_grd_total = 0;
  int m_grd_nfft = 0;
  // signal process
  float accumulated_time = 1; // second

  // result buffer
  const int NUM_SLOTS = 16;
  size_t SLOT_SIZE = 0; // 每槽字节数
  std::vector<HostRingSlot> m_hring;
  int m_hhead = 0;
  SubbandConfig *m_config;
  NoiseState m_noise_state = NoiseState::OFF;
  uint32_t m_acc_on_id = 0;
  uint32_t m_acc_off_id = 0;
  uint32_t acc_noise_blank = 0;
};
