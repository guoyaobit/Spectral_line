#include <arpa/inet.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <Globalcfg.hpp>
#include <fcntl.h>
#include <iostream>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vdif.hpp>
#include <vector>
#define nb_rxd_SIZE 8192
#define NUM_MBUFS 524288
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 256

auto &cfg = GlobalConfig::getInstance();
constexpr size_t EXPECTED_PKT_LEN = 8266;
struct lcore_param {
  uint16_t port_id;
  uint16_t queue_id;
  uint16_t lcore_id;
  uint16_t dest_port;
  uint16_t ring_id;
};

static struct rte_flow *create_udp_dst_flow(uint16_t port_id, uint16_t dst_port,
                                            uint16_t queue_id) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[4];
  struct rte_flow_action action[2];
  struct rte_flow_item_udp udp_spec, udp_mask;
  struct rte_flow_action_queue queue = {.index = queue_id};
  struct rte_flow_error error;
  struct rte_flow *flow = NULL;

  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;
  attr.priority = 0; // Highest priority

  // Match Ethernet/IPv4/UDP packets by destination port.
  memset(pattern, 0, sizeof(pattern));
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;

  memset(&udp_spec, 0, sizeof(udp_spec));
  memset(&udp_mask, 0, sizeof(udp_mask));
  udp_spec.hdr.dst_port = rte_cpu_to_be_16(dst_port);
  udp_mask.hdr.dst_port = 0xFFFF; // Exact destination-port match

  pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
  pattern[2].spec = &udp_spec;
  pattern[2].mask = &udp_mask;

  pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

  // Redirect matching packets to the selected receive queue.
  memset(action, 0, sizeof(action));
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  if (!flow) {
    cfg.logger_->error(
        "❌ Failed to create flow for UDP dport{} -> queue {}: {}\n", dst_port,
        queue_id, error.message ? error.message : "(no msg)");
  } else {
    cfg.logger_->debug("✅ Flow created: UDP dport {} -> queue {}\n", dst_port,
                       queue_id);
  }

  return flow;
}

static struct rte_flow *create_catch_all_drop(uint16_t port_id) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[2];
  struct rte_flow_action action[2];
  struct rte_flow_error error;
  struct rte_flow *flow = NULL;

  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;
  attr.priority = 2; // Lower priority than destination-port rules

  // Match all remaining Ethernet traffic.
  memset(pattern, 0, sizeof(pattern));
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  // Drop unmatched traffic.
  memset(action, 0, sizeof(action));
  action[0].type = RTE_FLOW_ACTION_TYPE_DROP;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  if (!flow) {
    cfg.logger_->debug("❌ Failed to create catch-all DROP flow: {}\n",
                       error.message ? error.message : "(no msg)");
  } else {
    cfg.logger_->debug(
        "✅ Catch-all DROP flow created (all other packets dropped)\n");
  }

  return flow;
}

static int port_init(uint16_t port, struct rte_mempool *mbuf_pool,
                     uint16_t nb_rx_queues) {

  uint16_t nb_rxd = nb_rxd_SIZE;
  int retval;
  struct rte_eth_conf port_conf{};
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  port_conf.rxmode.mtu = 9000;

  retval = rte_eth_dev_configure(port, nb_rx_queues, 1, &port_conf);
  if (retval < 0)
    return retval;

  for (uint16_t q = 0; q < nb_rx_queues; q++) {

    retval = rte_eth_rx_queue_setup(
        port, q, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
    if (retval < 0)
      return retval;
  }

  rte_eth_promiscuous_disable(port);
  rte_eth_allmulticast_disable(port);
  retval = rte_eth_dev_start(port);
  if (retval < 0)
    return retval;
  for (int i = 0; i < nb_rx_queues; i++) {
    create_udp_dst_flow(port, 60000 + i, i);
  }
  create_catch_all_drop(port);

  return 0;
}
static int locre_drop(void *arg) {
  struct lcore_param *param = (struct lcore_param *)arg;
  unsigned lcore_id = rte_lcore_id();
  uint16_t port = param->port_id;
  uint16_t queue_id = param->queue_id;
  struct rte_mbuf *bufs[BURST_SIZE];
  uint16_t nb_rx;
  auto &cfg = GlobalConfig::getInstance();
  cfg.logger_->debug(
      "Running locre_drop thread on port {} ,queue {} ,on core {}", port,
      queue_id, lcore_id);

  while (1) {
    nb_rx = rte_eth_rx_burst(port, queue_id, bufs, BURST_SIZE);
    if (nb_rx == 0)
      continue;

    for (int i = 0; i < nb_rx; i++) {
      struct rte_mbuf *mbuf = bufs[i];
      rte_pktmbuf_free(mbuf);
    }
  }
  return 0;
}
static int recv2mem(void *args) {
  struct lcore_param *param = (struct lcore_param *)args;

  const int stream_id = param->ring_id;
  const uint16_t port = param->port_id;
  const uint16_t queue_id = param->queue_id;
  auto &stream_cfg = cfg.streams[stream_id];
  auto &queue = stream_cfg.queue;
  auto &pool = stream_cfg.pool;

  size_t pool_idx = 0;
  uint64_t prebatchid = 0;

  cfg.logger_->debug("Running recv2mem thread: stream id :{}, core id: {}",
                     stream_id, rte_lcore_id());

  cfg.logger_->debug("Batchsize is {}", pool[0]->pkts.size());

  // Packets returned by one receive burst.
  rte_mbuf *bufs[BURST_SIZE];

  // Packets to release after processing the burst.
  rte_mbuf *free_bufs[BURST_SIZE];

  uint64_t expected_pkt_id = 0;
  uint64_t total_lostnmber = 0;
  uint64_t total_pkts = 0;
  const uint32_t batchsize = pool[0]->count;
  std::string monitor_data_path = "/dev/shm/server_" +
                                  std::to_string(cfg.ServerID) + "_stream_" +
                                  std::to_string(stream_id) + ".bin";

  constexpr size_t MONITOR_SIZE = 32 + 8192;

  int fd = open(monitor_data_path.c_str(), O_RDWR | O_CREAT, 0666);

  if (fd < 0) {
    perror("open monitor file");
    return -1;
  }

  if (ftruncate(fd, MONITOR_SIZE) != 0) {
    perror("ftruncate monitor file");
    close(fd);
    return -1;
  }

  void *monitor_map =
      mmap(nullptr, MONITOR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (monitor_map == MAP_FAILED) {
    perror("mmap monitor file");
    close(fd);
    return -1;
  }

  auto *monitor_ptr = static_cast<uint8_t *>(monitor_map);
  while (1) {
    /*
     * =========================================================
     * Receive one packet burst.
     * =========================================================
     */
    const uint16_t nb_rx = rte_eth_rx_burst(port, queue_id, bufs, BURST_SIZE);

    if (unlikely(nb_rx == 0))
      continue;
    unsigned int nb_free = 0;

    /*
     * =========================================================
     * Process the packet burst.
     * =========================================================
     */
    for (unsigned int i = 0; i < nb_rx; ++i) {
      rte_mbuf *mbuf = bufs[i];
      size_t datalen = rte_pktmbuf_pkt_len(mbuf);
      if (datalen != EXPECTED_PKT_LEN) {
        rte_pktmbuf_free(mbuf);
        continue;
      }

      /*
       * Prefetch four packets ahead into the CPU cache.
       */
      if (i + 4 < nb_rx) {
        rte_prefetch0(rte_pktmbuf_mtod(bufs[i + 4], void *));
      }

      total_pkts++;

      /*
       * =====================================================
       * VDIF header
       * =====================================================
       */
      VDIF header;

      uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);

      // Ethernet 14 + IPv4 20 + UDP 8 = 42
      uint8_t *vdif_ptr = data + 42;
      rte_memcpy(header.headerPtr(), vdif_ptr, 32);

      /*
       * Update the VDIF header for baseband output.
       */
      if (cfg.observation_mode == ObservationMode::BASEBAND) {
        header.setEDV(1);
        header.setFrameLength(8192 * 8 / cfg.Baseband_bits);
        header.setComplex(true);
        header.setBitsPerSample(cfg.Baseband_bits);
        header.setLog2Channels(0);
        header.setVDIFVersion(1);
        header.setThreadID(cfg.ServerID * 16 + stream_id);
      }
      /*
       * =====================================================
       * VDIF metadata
       * =====================================================
       */
      const uint64_t m_seconds = header.getSecondsFromEpoch();
      const uint64_t m_frame_number = header.getFrameNumber();
      const uint32_t m_NosieSoureState = header.getNoiseSourceState();

      /*
       * =====================================================
       * Noise source state
       * =====================================================
       */
      /*
       * =====================================================
       * subband monitor
       * =====================================================
       */
      if (unlikely(cfg.subband_monitor && m_seconds % 2 == 0 &&
                   m_frame_number == 0)) {
        rte_memcpy(monitor_ptr, vdif_ptr, 8224);
      }

      /*
       * =====================================================
       * packet ID
       * =====================================================
       */
      const uint64_t recv_packet_id = m_seconds * 62500ULL + m_frame_number;

      /*
       * =====================================================
       * batch ID
       * =====================================================
       */
      const uint64_t batchid = recv_packet_id / batchsize;

      const uint32_t pkt_idx_inbatch = recv_packet_id % batchsize;

      /*
       * =====================================================
       * packet sequence check
       * =====================================================
       *
       * A zero expected ID accepts the first complete batch. A mismatch after
       * initialization is counted as packet loss.
       */
      if (expected_pkt_id == 0) {
        if (unlikely(pkt_idx_inbatch != 0)) {
          free_bufs[nb_free++] = mbuf;
          continue;
        }
        expected_pkt_id = recv_packet_id;
        cfg.logger_->info("First packet_id is {} ,on stream {}, seconds is {}, "
                          "framenumber is {}",
                          expected_pkt_id, stream_id, m_seconds,
                          m_frame_number);

        expected_pkt_id++;
        prebatchid = batchid;
      } else {
        if (unlikely(expected_pkt_id != recv_packet_id)) {
          const int64_t lostnmber =
              static_cast<int64_t>(recv_packet_id - expected_pkt_id);

          total_lostnmber += lostnmber;

          cfg.logger_->warn("Stream {}:total_lostnmber {}, "
                            "m_seconds {}, m_frame_number {}, "
                            "recv_packet_id:{} , "
                            "expected_pkt_id: {} ,lost {} packets",
                            stream_id, total_lostnmber, m_seconds,
                            m_frame_number, recv_packet_id, expected_pkt_id,
                            lostnmber);

          expected_pkt_id = recv_packet_id + 1;

          if (cfg.Debug_mode) {
            double loss_rate =
                static_cast<double>(total_lostnmber) /
                static_cast<double>(total_lostnmber + total_pkts);

            std::time_t t = std::time(nullptr);

            std::cout << std::ctime(&t) << "stream: " << stream_id
                      << " lost: " << lostnmber
                      << " total_lost: " << total_lostnmber
                      << " total received: " << total_pkts
                      << " loss_rate: " << std::scientific
                      << std::setprecision(2) << loss_rate << std::endl;
          }
        } else {
          expected_pkt_id++;
        }
      }
      /*
       * =====================================================
       * Select the current batch.
       * =====================================================
       */
      PacketBatch *batch = pool[pool_idx];
      /*
       * =====================================================
       * Advance across completed batches.
       * =====================================================
       */
      if (unlikely(batchid != prebatchid)) {
        while (prebatchid < batchid) {
          /*
           * Enqueue the completed batch.
           */
          batch = pool[pool_idx];

          if (unlikely(!queue.try_enqueue(batch))) {
              cfg.logger_->warn("Pktdata queue {} is full; applying backpressure", stream_id);
              queue.wait_enqueue(batch);
            }

          prebatchid++;

          /*
           * Reuse the next batch in the pool.
           */
          pool_idx++;

          if (pool_idx >= pool.size())
            pool_idx = 0;

          batch = pool[pool_idx];

          batch->valid = true;
        }
      }

      /*
       * =====================================================
       * Store the packet metadata and payload.
       * =====================================================
       */
      Packet *pkt = batch->pkts[pkt_idx_inbatch];
      batch->pkt_id[pkt_idx_inbatch] = recv_packet_id;
      batch->noise_state[pkt_idx_inbatch] = m_NosieSoureState;
      batch->hdrs[pkt_idx_inbatch] = header;

      /*
       * VDIF payload
       *
       * mbuf:
       *
       * 42 bytes Ethernet/IP/UDP
       * 32 bytes VDIF header
       * 8192 bytes payload
       */
      rte_memcpy(pkt->payload, vdif_ptr + 32, 8192);

      /*
       * Defer release so the entire burst can be freed at once.
       */
      free_bufs[nb_free++] = mbuf;
    }

    /*
     * =========================================================
     * Release processed packets in one bulk operation.
     * =========================================================
     */
    if (nb_free != 0) {
      rte_pktmbuf_free_bulk(free_bufs, nb_free);
    }
  }

  return 0;
}
// Resolve a DPDK port identifier by device name.
inline int get_port_by_name(const std::string &name) {
  uint16_t nb_ports = rte_eth_dev_count_avail();
  for (uint16_t port = 0; port < nb_ports; ++port) {
    char port_name[RTE_ETH_NAME_MAX_LEN];
    if (rte_eth_dev_get_name_by_port(port, port_name) == 0) {
      if (name == port_name)
        return port;
    }
  }
  return -1;
}
std::vector<lcore_param>
generate_lcore_params(const std::vector<uint16_t> &port_ids,
                      uint16_t queues_per_port, uint16_t start_dest_port) {
  std::vector<lcore_param> params;
  std::vector<unsigned> next_lcore(RTE_MAX_NUMA_NODES, 1);
  std::vector<unsigned> enabled_lcores;

  unsigned lcore_id;
  RTE_LCORE_FOREACH(lcore_id) { enabled_lcores.push_back(lcore_id); }

  if (enabled_lcores.empty()) {
    throw std::runtime_error("No enabled lcores found!");
  }
  int m_ring_id = 0;
  for (auto port : port_ids) {
    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (port >= nb_ports) {
      throw std::runtime_error("Invalid port_id: " + std::to_string(port));
    }

    for (uint16_t q = 0; q < queues_per_port; ++q) {
      unsigned assigned_lcore = RTE_MAX_LCORE;
      uint16_t port_socket = rte_eth_dev_socket_id(port);

      // Prefer a logical core on the port's NUMA node.
      for (unsigned lc = next_lcore[port_socket]; lc < RTE_MAX_LCORE; ++lc) {
        if (rte_lcore_is_enabled(lc) &&
            rte_lcore_to_socket_id(lc) == port_socket) {
          assigned_lcore = lc;
          next_lcore[port_socket] = lc + 1;
          break;
        }
      }

      // Fall back to another enabled logical core.
      if (assigned_lcore == RTE_MAX_LCORE) {
        for (auto lc : enabled_lcores) {
          if (lc >= next_lcore[port_socket])
            continue; // Avoid assigning a core twice.
          assigned_lcore = lc;
          break;
        }
      }

      if (assigned_lcore == RTE_MAX_LCORE) {
        throw std::runtime_error("Not enough lcores for port " +
                                 std::to_string(port) + ", queue " +
                                 std::to_string(q));
      }

      lcore_param p;
      p.port_id = port;
      p.queue_id = q;
      p.ring_id = m_ring_id;
      m_ring_id++;
      p.lcore_id = assigned_lcore;
      p.dest_port = start_dest_port + q;
      params.push_back(p);
    }
  }

  return params;
}
int dpdk() {
  char arg0[] = "7mm_recv";
    char arg1[] = "-n";
    char arg2[] = "4";
    char *argv[] = {arg0, arg1, arg2, nullptr};
    const int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0])) - 1;
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Error with EAL init\n");

  struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
      "MBUF_POOL", NUM_MBUFS * 2, MBUF_CACHE_SIZE, 0, 10240, rte_socket_id());
  if (mbuf_pool == NULL)
    rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
  auto &cfg = GlobalConfig::getInstance();
  uint16_t queues_per_port = 8;
  uint16_t start_dest_port = 60000;
  std::vector<uint16_t> ports = {0, 1};
  // Initialize both physical receive ports.
  if (port_init(0, mbuf_pool, queues_per_port) != 0)
    rte_exit(EXIT_FAILURE, " Cannot init port %" PRIu16 "\n", 0);
  if (port_init(1, mbuf_pool, queues_per_port) != 0)
    rte_exit(EXIT_FAILURE, " Cannot init port %" PRIu16 "\n", 0);

  auto lcore_params =
      generate_lcore_params(ports, queues_per_port, start_dest_port);
  for (size_t i = 0; i < lcore_params.size(); ++i) {
      const size_t subband_index = i / 2; // Two queues per subband
    if (cfg.subbands[subband_index]->enable) {
      rte_eal_remote_launch(recv2mem, &lcore_params[i],
                            lcore_params[i].lcore_id);
    } else {
      rte_eal_remote_launch(locre_drop, &lcore_params[i],
                            lcore_params[i].lcore_id);
    }
  }
  rte_eal_mp_wait_lcore();
  return 0;
}
