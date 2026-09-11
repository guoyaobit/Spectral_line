#include <DoPFBFFT.hpp>
#include <Globalcfg.hpp>
#include <cuda_runtime.h>
#include <iostream>
#include <malloc.h>   // for aligned_alloc
#include <sys/mman.h> // for mlock, madvise
#include <unistd.h>   // for sysconf
#include <baseband.hpp>
#include <complex>
#include <omp.h>
#include <thread>
// P-tap PFB prototype filter
// N = FFT_size * P
// h[n] = sinc * Hann
void genPfbWin(std::vector<float> &win, int N, int P) {
  if ((int)win.size() < N)
    throw std::runtime_error("win size too small!");

  const double center = (N - 1) * 0.5;

  double sum = 0.0;

  // Generate a sinc-Hann prototype filter.
  for (int i = 0; i < N; i++) {
    double x = i - center;

    // sinc(pi*x*P/N)
    double arg = M_PI * x * P / N;

    double sinc;

    if (fabs(arg) < 1e-12) {
      sinc = 1.0;
    } else {
      sinc = sin(arg) / arg;
    }

    double hann = 0.5 - 0.5 * cos(2.0 * M_PI * i / (N - 1));

    double h = sinc * hann;

    win[i] = static_cast<float>(h);

    sum += h;
  }

  // Normalize the DC gain.
  for (int i = 0; i < N; i++) {
    win[i] /= static_cast<float>(sum);
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
  for (size_t i = 0; i < numpkts; i++) {
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
  auto &cfg = GlobalConfig::getInstance();
  cudaSetDevice(cfg.subbands[subband_id]->gpu_id);
  {
    std::lock_guard<std::mutex> lock(cfg.init_mutex);
    cfg.ready_threads++;
  }
  cfg.init_cv.notify_all();
  cfg.logger_->info("subband {} ready {}/{}", subband_id, cfg.ready_threads,
                    cfg.total_threads);
  {
    cfg.logger_->info("subband {}: spectrum line mode, PFB window generated",
                      subband_id);
    size_t num_taps = 4;
    // PFB/FFT parameters.
    int Nfft = cfg.total_nfft;
    std::vector<float> pfbwin(num_taps * Nfft);
    genPfbWin(pfbwin, num_taps * Nfft, num_taps);
    std::unique_ptr<GpuPfbFft> obj =
        std::make_unique<GpuPfbFft>(subband_id, pfbwin.data());
    while (true) {
      obj->accumulate_one_block(); // Asynchronous CPU-to-GPU copy
      obj->submit_PFB_FFT();       // PFB+ FFT
      obj->StokesAcc();            // Stokes kernel
    }
  }
}
void baseband_thread(int streamid) {
  auto &cfg = GlobalConfig::getInstance();
  std::unique_ptr<baseband> baseband_obj = std::make_unique<baseband>(streamid);
  {
    std::lock_guard<std::mutex> lock(cfg.init_mutex);
    cfg.ready_threads++;
  }
  cfg.init_cv.notify_all();
  cfg.logger_->info("BASEBAND {}: ready {}/{}", streamid, cfg.ready_threads,
                    cfg.total_threads);
  while (true) {
    baseband_obj->recoder();
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
  // Count workers that must complete initialization.
  size_t enabled_subbands = 0;
  for (size_t i = 0; i < cfg.subbands.size(); i++) {
    if (!cfg.subbands[i]->enable)
      continue;

    enabled_subbands++;
  }
  if (cfg.observation_mode == ObservationMode::BASEBAND) {
    cfg.total_threads = enabled_subbands * 2;
  } else
    cfg.total_threads = enabled_subbands;

  cfg.QUEUE_CAPACITY = calc_pool_size();

  cfg.streams.reserve(cfg.max_streams);

  size_t enabled_streams = 0;

  // Map receive streams to enabled subbands.
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

  // Allocate packet memory only for enabled streams.
  uint64_t total_packets =
      static_cast<uint64_t>(enabled_streams) * cfg.QUEUE_CAPACITY * batchsize;

  uint64_t total_bytes = total_packets * sizeof(Packet);

  cfg.logger_->info("QUEUE_CAPACITY = {}", cfg.QUEUE_CAPACITY);

  cfg.logger_->info("Enabled streams = {}", enabled_streams);
  // Baseband recording does not require CUDA-pinned host memory.
  if (cfg.observation_mode == ObservationMode::SPECTRAL ||
      cfg.observation_mode == ObservationMode::CONTINUUM) {
    cfg.logger_->info("Allocating {:.2f} GiB pinned memory ({} bytes)",
                      static_cast<double>(total_bytes) / 1024 / 1024 / 1024,
                      total_bytes);

    cudaError_t err =
        cudaHostAlloc((void **)&cfg.packet_pool, total_bytes,
                      cudaHostAllocPortable | cudaHostAllocWriteCombined);

    if (err != cudaSuccess) {
      throw std::runtime_error(cudaGetErrorString(err));
    }
  } else {
    cfg.packet_pool = static_cast<Packet *>(aligned_alloc(64, total_bytes));
    if (cfg.packet_pool == nullptr) {
      throw std::runtime_error("aligned_alloc failed");
    }
    if (mlock(cfg.packet_pool, total_bytes) != 0) {
      std::cerr << "Warning: mlock failed, performance may degrade"
                << std::endl;
    }
  }
  memset(cfg.packet_pool, 0, total_bytes);

  // Map PacketBatch objects onto the contiguous packet pool.
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

  // Start one worker per active baseband stream or spectral subband.
  if (cfg.observation_mode == ObservationMode::BASEBAND) {
    for (int i = 0; i < cfg.max_streams; ++i) {
      int subband_id = i / 2;
      if (!cfg.subbands[subband_id]->enable) {
        cfg.logger_->info("subband {} is disabled, skip stream {}.", subband_id,
                          i);
        continue;
      }
      std::thread t(baseband_thread, i);
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      unsigned cpu_id = cfg.max_streams + 1 + i;
      CPU_SET(cpu_id, &cpuset);
      pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
      cfg.logger_->info("BASEBAND {}: thread pinned to CPU {}", i, cpu_id);
      t.detach();
    }
  } else {
    for (size_t i = 0; i < cfg.subbands.size(); ++i) {
      if (!cfg.subbands[i]->enable) {
        cfg.logger_->info("subband {} is disabled, skip it.", i);
        continue;
      }
      cfg.logger_->info("subband {}: starting thread on GPU {}", i,
                        cfg.subbands[i]->gpu_id);

      std::thread t(subband_thread, i);
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      unsigned cpu_id = cfg.max_streams + 1 + i;
      CPU_SET(cpu_id, &cpuset);
      pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
      cfg.logger_->info("subband {}: thread pinned to CPU {}", i, cpu_id);
      t.detach();
    }
  }

  // Remove stale monitor files before receiving new frames.
  for (int stream_id = 0; stream_id < cfg.max_streams; ++stream_id) {
    std::string path = "/dev/shm/server_" + std::to_string(cfg.ServerID) +
                       "_stream_" + std::to_string(stream_id) + ".bin";

    unlink(path.c_str());
  }
  return 0;
}
