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
  if (cfg.observation_mode == 0 || cfg.observation_mode == 2) {
    cfg.total_nfft = 65536;
  }
  // init memory
  init();
  std::unique_lock<std::mutex> lock(cfg.init_mutex);
  cfg.init_cv.wait(lock,
                   [&cfg] { return cfg.ready_threads == cfg.total_threads; });
  cfg.logger_->info("All subband threads are ready");
  cfg.ready_threads = 0;
  cfg.total_threads = cfg.subbands.size() * 2;
  // start dpdk thread
  pthread_t dpdk_t;
  pthread_create(&dpdk_t, NULL, dpdk_thread, NULL);
  pthread_join(dpdk_t, NULL);
  // while(1)
  // {

  // }
  // cfg.logger_->info("quit\n");

  return 0;
}