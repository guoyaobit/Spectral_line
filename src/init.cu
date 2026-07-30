#include <DoPFBFFT.hpp>
#include <Globalcfg.hpp>
#include <cuda_runtime.h>
#include <iostream>
// #include <GpuStokes.h>
#include <baseband.hpp>
#include <complex>
#include <omp.h>
#include <thread>
// 生成长度 N 的 sinc 窗函数 (归一化), 存到 win
void genPfbWin(std::vector<float> &win, int N, int P) {
  if ((int)win.size() < N) {
    throw std::runtime_error("win size too small!");
  }

  float center = 1.0f * (N - 1) / 2.0f;
  float sum = 0.0f;

  // 生成 sinc 窗
  for (int i = 0; i < N; i++) {
    float t = i - center;
    float val;
    if (fabs(t) < 1e-6f) {
      val = 1.0f;
    } else {
      val = sinf(M_PI * t * P / N) / (M_PI * t * P / N);
    }
    win[i] = val;
    sum += val;
  }
  // 归一化
  for (int i = 0; i < N; i++) {
    win[i] /= sum;
  }
}
PacketBatch *allocatePacketBatch(Packet *buffer, size_t numpkts) {
  PacketBatch *batch = new PacketBatch;

  batch->count = numpkts;
  batch->buffer = buffer;

  batch->pkts.resize(numpkts);
  batch->hdrs.resize(numpkts);
  batch->pkt_id.resize(numpkts);
  batch->noise_state.resize(numpkts);
  batch->timestamps.resize(numpkts);
  for (size_t i = 0; i < numpkts; i++)
  {
	  batch->pkts[i] = &batch->buffer[i];
  }
  return batch;
}
void freeDataBatch(PacketBatch *batch) {
  if (!batch)
    return;
  cudaFreeHost(batch->buffer);
  delete batch;
}

void subband_thread(int subband_id) {
  float4 *result;
  auto &cfg = GlobalConfig::getInstance();
  cudaSetDevice(cfg.subbands[subband_id]->gpu_id);
  {
    std::lock_guard<std::mutex> lock(cfg.init_mutex);
    cfg.ready_threads++;
  }
  cfg.init_cv.notify_all();
  cfg.logger_->info("subband {} ready {}/{}", subband_id, cfg.ready_threads,
                    cfg.total_threads);
  if (cfg.observation_mode == 0) // baseband mode
  {
    cfg.logger_->info("subband {}: baseband mode, no PFB window generated",
                      subband_id);
    std::unique_ptr<baseband> baseband_obj =
        std::make_unique<baseband>(subband_id);
    while (true) {
      baseband_obj->collectdata();
    }
  } else if (cfg.observation_mode == 1) // spectrum line mode
  {
    cfg.logger_->info("subband {}: spectrum line mode, PFB window generated",
                      subband_id);
    size_t num_taps = 4;
    // PfB+ FFT para
    int Nfft = cfg.total_nfft;
    std::vector<float> pfbwin(num_taps * Nfft);
    genPfbWin(pfbwin, num_taps * Nfft, num_taps);
    std::unique_ptr<GpuPfbFft> obj =
        std::make_unique<GpuPfbFft>(subband_id, pfbwin.data(), result);
    while (true) {
      obj->accumulate_one_block(); // CPU -> GPU 异步拷贝
      obj->submit_PFB_FFT();       // PFB+ FFT
      obj->StokesAcc();            // Stokes kernel
    }
  } else {
    cfg.logger_->error("subband {}: unknown observation mode {}", subband_id,
                       cfg.observation_mode);
    return;
  }
}

size_t calc_pool_size() {

  auto &cfg = GlobalConfig::getInstance();
  uint64_t MAX_POOL_MEMORY =
      cfg.Memory_pool_per_stream * 1024ULL * 1024ULL * 1024ULL;
  size_t batch_bytes = sizeof(Packet) * cfg.batchsize();

  size_t pool_size = MAX_POOL_MEMORY / batch_bytes;
  return std::max<size_t>(pool_size, 2);
}
int init() {
  auto &cfg = GlobalConfig::getInstance();
  // thread synchronization
  size_t enabled_subbands = 0;
  for (size_t i = 0; i < cfg.subbands.size(); i++) {
    if (!cfg.subbands[i]->enable)
      continue;

    enabled_subbands++;
  }
  cfg.total_threads = enabled_subbands;

  cfg.QUEUE_CAPACITY = calc_pool_size();

  cfg.streams.reserve(cfg.max_streams);

  size_t enabled_streams = 0;

  // 1. 初始化 stream 映射
  for (size_t s = 0; s < cfg.max_streams; ++s) {
    cfg.streams.emplace_back(cfg.QUEUE_CAPACITY);

    auto &stream = cfg.streams.back();

    stream.subband_id = s / 2;

    if (!cfg.subbands[stream.subband_id]->enable) {
      cfg.logger_->info("stream {} disabled (subband {})", s,
                        stream.subband_id);

      continue;
    }

    stream.enable = true;
    stream.pool_index = enabled_streams++;
  }

  cfg.enabled_streams = enabled_streams;

  int batchsize = cfg.batchsize();

  // 2. 只为启用的 stream 分配 pinned memory
  uint64_t total_packets =
      static_cast<uint64_t>(enabled_streams) * cfg.QUEUE_CAPACITY * batchsize;

  uint64_t total_bytes = total_packets * sizeof(Packet);

  cfg.logger_->info("QUEUE_CAPACITY = {}", cfg.QUEUE_CAPACITY);

  cfg.logger_->info("Enabled streams = {}", enabled_streams);

  cfg.logger_->info("Allocating {:.2f} GiB pinned memory ({} bytes)",
                    static_cast<double>(total_bytes) / 1024 / 1024 / 1024,
                    total_bytes);

  cudaError_t err = cudaMallocHost((void **)&cfg.packet_pool, total_bytes);

  if (err != cudaSuccess) {
    throw std::runtime_error(cudaGetErrorString(err));
  }

  // 3. 建立 PacketBatch 到连续内存的映射
  for (size_t s = 0; s < cfg.max_streams; ++s) {
    auto &stream = cfg.streams[s];

    if (!stream.enable)
      continue;

    stream.pool.resize(cfg.QUEUE_CAPACITY);

    for (size_t b = 0; b < cfg.QUEUE_CAPACITY; ++b) {
      uint64_t offset =
          (static_cast<uint64_t>(stream.pool_index) * cfg.QUEUE_CAPACITY + b) *
          batchsize;

      Packet *buffer = cfg.packet_pool + offset;

      stream.pool[b] = allocatePacketBatch(buffer, batchsize);
    }
  }

  // 4. 启动子带线程
  for (size_t i = 0; i < cfg.subbands.size(); ++i) {
    if (!cfg.subbands[i]->enable) {
      cfg.logger_->info("subband {} is disabled, skip it.", i);
      continue;
    }
    cfg.logger_->info("subband {}: starting thread on GPU {}", i,
                      cfg.subbands[i]->gpu_id);

    std::thread t(subband_thread, i);

    t.detach();
  }

  return 0;
}
