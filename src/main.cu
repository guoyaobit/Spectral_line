#include <Globalcfg.hpp>
#include <cerrno>
#include <cstring>
#include <inttypes.h>
#include <iostream>
#include <pthread.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
extern int dpdk();

static void remove_stale_monitor_files(GlobalConfig &cfg) {
  size_t removed_files = 0;
  for (int stream_id = 0; stream_id < cfg.max_streams; ++stream_id) {
    const std::string path =
        "/dev/shm/server_" + std::to_string(cfg.ServerID) + "_stream_" +
        std::to_string(stream_id) + ".bin";

    if (unlink(path.c_str()) == 0) {
      removed_files++;
    } else if (errno != ENOENT) {
      cfg.logger_->warn("Cannot remove stale monitor file {}: {}", path,
                        std::strerror(errno));
    }
  }
  cfg.logger_->info("Removed {} stale monitor file(s) for server {}",
                    removed_files, cfg.ServerID);
}

// DPDK thread entry point.
void *dpdk_thread(void *) {
  dpdk();
  return nullptr;
}
extern int init();
int main() {
  auto &cfg = GlobalConfig::getInstance();
  cfg.initlog();
  try {
    if (!cfg.initFromYaml("config.yaml")) {
      return 1;
    }
    remove_stale_monitor_files(cfg);
    if (cfg.observation_mode == ObservationMode::BASEBAND) {
      if (cfg.Baseband_bits == 8) {
        cfg.total_nfft = 8192 * 256; // 8192 * 256
      } else if (cfg.Baseband_bits == 4) {
        cfg.total_nfft = 8192 * 256; // 8192 * 512
      } else {
        cfg.total_nfft = 8192 * 512; // 8192 * 512
      }
    }
    init();
    std::unique_lock<std::mutex> lock(cfg.init_mutex);
    cfg.init_cv.wait(lock,
                     [&cfg] { return cfg.ready_threads == cfg.total_threads; });
    cfg.logger_->info("All subband threads are ready");
    pthread_t dpdk_t;
    pthread_create(&dpdk_t, NULL, dpdk_thread, NULL);
    pthread_join(dpdk_t, NULL);
    return 0;
  } catch (const std::exception &e) {
    cfg.logger_->critical("Fatal initialization error: {}", e.what());
    return 1;
  }
}
