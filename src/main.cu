#include <Globalcfg.hpp>
#include <inttypes.h>
#include <iostream>
#include <pthread.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
extern int dpdk();

// 线程入口函数
void *dpdk_thread(void *arg) {
  int ret = dpdk();
  return NULL;
}
extern int init();
int main() {
  // init global config
  auto &cfg = GlobalConfig::getInstance();
  cfg.initlog();
  // read cfg from yaml
  if (!cfg.initFromYaml("config.yaml")) {
    return false;
  }
  if (cfg.observation_mode == ObservationMode::BASEBAND) {
    if (cfg.Baseband_bits == 8) {
      cfg.total_nfft = 8192 * 256; // 8192 * 256
    } else if (cfg.Baseband_bits == 4) {
      cfg.total_nfft = 8192 * 256; // 8192 * 512
    } else {
      cfg.total_nfft = 8192 * 512; // 8192 * 512
    }
  }
  // init memory
  init();
  std::unique_lock<std::mutex> lock(cfg.init_mutex);
  cfg.init_cv.wait(lock,
                   [&cfg] { return cfg.ready_threads == cfg.total_threads; });
  cfg.logger_->info("All subband threads are ready");
  // start dpdk thread
  pthread_t dpdk_t;
  pthread_create(&dpdk_t, NULL, dpdk_thread, NULL);
  pthread_join(dpdk_t, NULL);

  return 0;
}