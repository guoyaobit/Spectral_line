#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <Globalcfg.hpp>
#include <iostream>
#include <vector>

#include "VDIFReader.hpp"
#include "readerwriterqueue.h"
#include "readerwritercircularbuffer.h"
#include <chrono>

#define RX_RING_SIZE 4096
#define NUM_MBUFS 262144
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 128
#define RING_SIZE 4096

auto &cfg = GlobalConfig::getInstance();
constexpr size_t EXPECTED_PKT_LEN = 8266;
std::vector<rte_ring *> rx_rings;
struct lcore_param
{
    uint16_t port_id;
    uint16_t queue_id;
    uint16_t lcore_id;
    uint16_t dest_port;
    uint16_t ring_id;
};
static inline uint32_t simple_port_hash(uint16_t dst_port)
{
    return (uint32_t)dst_port;
}

static struct rte_flow *
create_udp_dst_flow(uint16_t port_id, uint16_t dst_port, uint16_t queue_id)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[4];
    struct rte_flow_action action[2];
    struct rte_flow_item_udp udp_spec, udp_mask;
    struct rte_flow_action_queue queue = {.index = queue_id};
    struct rte_flow_error error;
    struct rte_flow *flow = NULL;

    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;  // 入方向流量
    attr.priority = 0; // 优先级（0最高）

    // pattern: ETH → IPv4 → UDP (目的端口匹配)
    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;

    memset(&udp_spec, 0, sizeof(udp_spec));
    memset(&udp_mask, 0, sizeof(udp_mask));
    udp_spec.hdr.dst_port = rte_cpu_to_be_16(dst_port);
    udp_mask.hdr.dst_port = 0xFFFF; // 完全匹配端口

    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;

    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    // action: 重定向到指定队列
    memset(action, 0, sizeof(action));
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    if (!flow)
    {
        cfg.logger_->error("❌ Failed to create flow for UDP dport{} -> queue {}: {}\n",
                           dst_port, queue_id, error.message ? error.message : "(no msg)");
    }
    else
    {
        cfg.logger_->debug("✅ Flow created: UDP dport {} -> queue {}\n", dst_port, queue_id);
    }

    return flow;
}

static struct rte_flow *
create_catch_all_drop(uint16_t port_id)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[2];
    struct rte_flow_action action[2];
    struct rte_flow_error error;
    struct rte_flow *flow = NULL;

    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;
    attr.priority = 2; // 低优先级，最后匹配

    // 匹配所有
    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    // 动作 = 丢弃
    memset(action, 0, sizeof(action));
    action[0].type = RTE_FLOW_ACTION_TYPE_DROP;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    if (!flow)
    {
        cfg.logger_->debug("❌ Failed to create catch-all DROP flow: {}\n",
                           error.message ? error.message : "(no msg)");
    }
    else
    {
        cfg.logger_->debug("✅ Catch-all DROP flow created (all other packets dropped)\n");
    }

    return flow;
}

static int
port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t nb_rx_queues)
{

    uint16_t nb_rxd = RX_RING_SIZE;
    int retval;

    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(port, &dev_info);
    struct rte_eth_conf port_conf = {0};
    /* RX 多队列模式 */
    port_conf.rxmode.mtu = 9000;
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    retval = rte_eth_dev_configure(port, nb_rx_queues, nb_rx_queues, &port_conf);
    if (retval < 0)
        return retval;

    struct rte_eth_rxconf rxconf;
    rxconf = dev_info.default_rxconf;
    rxconf.rx_free_thresh = 1024;
    for (uint16_t q = 0; q < nb_rx_queues; q++)
    {

        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
        retval = rte_eth_tx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL);
        if (retval < 0)
            return retval;
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;
    for (int i = 0; i < nb_rx_queues; i++)
    {
        create_udp_dst_flow(port, 60000 + i, i);
    }
    // catch-all
    // create_catch_all_flow(port,i+1);
    create_catch_all_drop(port);
    // printf("🚀 Port %d ready, listening on UDP 60000-60003\n", port);
    // disable promisc
    rte_eth_promiscuous_disable(port);
    rte_eth_allmulticast_disable(port);
    return 0;
}
static int
locre_drop(void *arg)
{
    struct lcore_param *param = (struct lcore_param *)arg;
    unsigned lcore_id = rte_lcore_id();
    uint16_t port = param->port_id;
    uint16_t queue_id = param->queue_id;
    struct rte_mbuf *bufs[BURST_SIZE];
    uint16_t nb_rx;
    auto &cfg = GlobalConfig::getInstance();
    cfg.logger_->debug("Running locre_drop thread on port {} ,queue {} ,on core {}", port, queue_id, lcore_id);

    while (1)
    {
        nb_rx = rte_eth_rx_burst(port, queue_id, bufs, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        for (int i = 0; i < nb_rx; i++)
        {
            struct rte_mbuf *mbuf = bufs[i];
            rte_pktmbuf_free(mbuf);
        }
    }
    return 0;
}
// packet recving  thread
static int
lcore_recv(void *arg)
{
    struct lcore_param *param = (struct lcore_param *)arg;
    unsigned lcore_id = rte_lcore_id();
    uint16_t port = param->port_id;
    uint16_t queue_id = param->queue_id;
    struct rte_mbuf *bufs[BURST_SIZE];
    uint16_t nb_rx;
    uint64_t t_num = 0;
    auto &cfg = GlobalConfig::getInstance();
    size_t pool_idx = 0;
    cfg.logger_->debug("Running locre_recv thread on port {} ,queue {} ,on core {}", port, queue_id, lcore_id);

    while (1)
    {
        nb_rx = rte_eth_rx_burst(port, queue_id, bufs, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        for (int i = 0; i < nb_rx; i++)
        {
            struct rte_mbuf *mbuf = bufs[i];
            size_t datalen = rte_pktmbuf_pkt_len(mbuf);
            if (datalen != EXPECTED_PKT_LEN)
            {
                // printf("Droping packet data len = %d\n", datalen);
                rte_pktmbuf_free(mbuf);
                continue;
            }
            /**prase udp header */
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
            {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
            if (ip_hdr->next_proto_id != IPPROTO_UDP)
            {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((unsigned char *)ip_hdr + sizeof(struct rte_ipv4_hdr));
            //
            if (rte_be_to_cpu_32(ip_hdr->dst_addr) != 0x0A11100B)
            {

                rte_pktmbuf_free(mbuf);
            }

            uint16_t dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);

            // if (queue_id < cfg.max_streams)
            if (dst_port == param->dest_port)
            {
                rte_ring *ring = rx_rings[param->ring_id];
                int ret = rte_ring_enqueue(ring, mbuf);
                if (ret < 0)
                    cfg.logger_->info("Enqueue ring is full, ring_id: {}", param->ring_id);
            }
            else
            {
                // printf("%d,%d\n",dst_port,param->dest_port);
                rte_pktmbuf_free(mbuf);
            }
        }
    }
    return 0;
}

static int
recv2mem(void *args)
{
    struct lcore_param *param = (struct lcore_param *)args;
    int stream_id = param->ring_id;
    rte_ring *ring = rx_rings[param->ring_id];
    rte_mbuf *mbuf;
    auto &queue = cfg.streams[stream_id].queue;
    auto &pool = cfg.streams[stream_id].pool;
    size_t pool_idx = 0, pkt_idx_inbatch = 0;
    // FILE *logfile = nullptr;
    // char buf[64];
    // sprintf(buf, "sub_bands_%d.csv", param->queue_id);
    // logfile = init_log_append(buf);
    cfg.logger_->debug("Running recv2mem thread: stream id :{},core id: {}", stream_id, rte_lcore_id());
    uint64_t expected_pkt_id = 0;
    cfg.logger_->debug("Batchsize is {}", pool[0]->pkts.size());
    struct rte_mbuf *bufs[BURST_SIZE];
    int tx_idx = 0;
    uint64_t total_lostnmber = 0;
    uint64_t total_pkts = 0;
    uint32_t pre_NosieSoureState = 0xff; // magic number, means uninitialized state
    static auto start_time = std::chrono::steady_clock::now();
    static auto last_change_time = start_time;
    double duration;
    bool cal_vaild = false;
    uint64_t prebatchid = 0;
    while (1)
    {
        int ret = rte_ring_dequeue(ring, (void **)&mbuf);
        if (ret != 0)
        {
            continue;
        }
        total_pkts++;
        VDIFPacket header(rte_pktmbuf_mtod(mbuf, uint8_t *) + 42, 32);
        uint64_t m_seconds = header.getSecondsFromEpoch();
        uint64_t m_frame_number = header.getFrameNumber();
        uint64_t m_timestamp = header.getTimestamp();
        uint32_t m_NosieSoureState = header.getNoiseSourceSate();
        if (m_NosieSoureState != 0 && m_NosieSoureState != 1)
        {
            cfg.logger_->warn("Stream {}: Invalid Noise Source State: {}, m_seconds {}, m_frame_number {},m_timestamp:{}",
                              stream_id, m_NosieSoureState, m_seconds, m_frame_number, m_timestamp);
            // Invalid Noise Source State,drop packets
            continue;
        }
        if (cfg.subband_monitor && m_timestamp == 0)
        {
            std::string monitor_data_path =
                "/dev/shm/subband_" + std::to_string(stream_id) + ".bin";
            int fd = open(monitor_data_path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC,
                          0666);

            if (fd >= 0)
            {
                write(fd,
                      rte_pktmbuf_mtod(mbuf, void *),
                      rte_pktmbuf_pkt_len(mbuf));
                close(fd);
            }
        }
        if (cfg.cal_mode)
        {
            if (pre_NosieSoureState == 0xff)
                pre_NosieSoureState = m_NosieSoureState;
            if (pre_NosieSoureState != m_NosieSoureState) // state changed
            {
                if (cfg.Debug_mode)
                {
                    auto now = std::chrono::steady_clock::now();
                    duration = std::chrono::duration<double>(now - last_change_time).count();
                    std::cout << duration << "s ,Noise State change to : " << m_NosieSoureState << std::endl;
                    pre_NosieSoureState = m_NosieSoureState;
                    last_change_time = now;
                    cfg.logger_->info("last state duration is {} s", duration);
                }
                if (!cal_vaild) // first state change, start to calculate
                    cal_vaild = true;
            }
            if (!cal_vaild) // state not change, and first state not change, not start to calculate
            {
                rte_pktmbuf_free(mbuf);
                continue;
            }
        }

        // log_packet(logfile, m_seconds, m_frame_number);
        // fflush(logfile);
        uint64_t recv_packet_id = m_seconds * 62500ULL + m_frame_number;
        uint batchsize = pool[0]->count;
        // 计算当前包属于哪个 batch
        uint64_t batchid = recv_packet_id / batchsize;
        // 计算当前包在 batch 中的索引
        pkt_idx_inbatch = recv_packet_id % batchsize;
        if (expected_pkt_id == 0)
        {
            // first received packet
            expected_pkt_id = recv_packet_id;
            // std::cout <<"First frameid is"<<expected_pkt_id<<std::endl;
            cfg.logger_->info("First packet_id is {} ,on stream {}", expected_pkt_id, stream_id);
            expected_pkt_id++;
            prebatchid = batchid;
        }
        else
        {
            if (expected_pkt_id != recv_packet_id)
            {
                uint64_t lostnmber = recv_packet_id - expected_pkt_id;
                total_lostnmber += lostnmber;
                cfg.logger_->warn("Stream {}:total_lostnmber {},m_seconds {}, m_frame_number {},recv_packet_id:{} , expected_pkt_id: {} ,lost {} packets",
                                  stream_id, total_lostnmber, m_seconds, m_frame_number, recv_packet_id, expected_pkt_id, lostnmber);
                expected_pkt_id = recv_packet_id + 1;
                double loss_rate = static_cast<double>(total_lostnmber) / (total_lostnmber + total_pkts);
                if (cfg.Debug_mode)
                {
                    std::time_t t = std::time(nullptr);
                    std::cout << std::ctime(&t)

                              << "stream: " << stream_id

                              << " lost: " << lostnmber

                              << " total_lost: " << total_lostnmber

                              << " total received: " << total_pkts

                              << " loss_rate: " << std::scientific << std::setprecision(2) << loss_rate

                              << std::endl;
                }
            }
            else
            {
                expected_pkt_id++;
            }
        }
        PacketBatch *batch = pool[pool_idx]; // 获取当前 batch
        if (batchid != prebatchid)
        {
            while (prebatchid < batchid)
            {
                batch = pool[pool_idx]; // 获取当前 batch
                // 将 batch 放入队列
                if (!queue.try_enqueue(batch))
                {
                    cfg.logger_->error("Pktdata to Queue {} is full and overwrite", stream_id);
                }
                prebatchid++;
                // 获取下一个 batch
                pool_idx = (pool_idx + 1) % pool.size();                 // 循环使用 pool
                batch = pool[pool_idx];                                  // 更新 batch 指针
                memset(batch->buffer, 0, sizeof(Packet) * batch->count); // 清空 batch buffer
                memset(batch->pkt_id.data(), 0, sizeof(uint) * batch->count);
                memset(batch->noise_state.data(), 0, sizeof(uint8_t) * batch->count);
                memset(batch->timestamps.data(), 0, sizeof(uint64_t) * batch->count);
                batch->valid = true; // 重置 batch 的有效性
            }
        }
        Packet *pkt = batch->pkts[pkt_idx_inbatch];
        batch->pkt_id[pkt_idx_inbatch] = recv_packet_id;
        batch->noise_state[pkt_idx_inbatch] = m_NosieSoureState;
        batch->timestamps[pkt_idx_inbatch] = m_timestamp;
        rte_memcpy((void *)(batch->hdrs[pkt_idx_inbatch].data()),rte_pktmbuf_mtod(mbuf, uint8_t *) + 42,32);
        // copy one packet data to struct
        rte_memcpy(pkt->payload, rte_pktmbuf_mtod(mbuf, uint8_t *) + 42 + 32, 8192);
        rte_pktmbuf_free(mbuf);
    }
}
// 获取指定网卡的端口号
inline int get_port_by_name(const std::string &name)
{
    uint16_t nb_ports = rte_eth_dev_count_avail();
    for (uint16_t port = 0; port < nb_ports; ++port)
    {
        char port_name[RTE_ETH_NAME_MAX_LEN];
        if (rte_eth_dev_get_name_by_port(port, port_name) == 0)
        {
            if (name == port_name)
                return port;
        }
    }
    return -1;
}
std::vector<lcore_param> generate_lcore_params(const std::vector<uint16_t> &port_ids,
                                               uint16_t queues_per_port,
                                               uint16_t start_dest_port,
                                               bool include_lcore0 = false)
{
    std::vector<lcore_param> params;
    std::vector<unsigned> next_lcore(RTE_MAX_NUMA_NODES, include_lcore0 ? 0 : 1);

    std::vector<unsigned> enabled_lcores;
    unsigned lcore_id;
    RTE_LCORE_FOREACH(lcore_id)
    {
        enabled_lcores.push_back(lcore_id);
    }

    if (enabled_lcores.empty())
    {
        throw std::runtime_error("No enabled lcores found!");
    }
    int m_ring_id = 0;
    for (auto port : port_ids)
    {
        uint16_t nb_ports = rte_eth_dev_count_avail();
        if (port >= nb_ports)
        {
            throw std::runtime_error("Invalid port_id: " + std::to_string(port));
        }

        for (uint16_t q = 0; q < queues_per_port; ++q)
        {
            unsigned assigned_lcore = RTE_MAX_LCORE;
            uint16_t port_socket = rte_eth_dev_socket_id(port);

            // 优先在同 NUMA 节点分配
            for (unsigned lc = next_lcore[port_socket]; lc < RTE_MAX_LCORE; ++lc)
            {
                if (rte_lcore_is_enabled(lc) &&
                    rte_lcore_to_socket_id(lc) == port_socket)
                {
                    assigned_lcore = lc;
                    next_lcore[port_socket] = lc + 1;
                    break;
                }
            }

            // 如果该 NUMA 节点不足，从其他 NUMA 节点分配
            if (assigned_lcore == RTE_MAX_LCORE)
            {
                for (auto lc : enabled_lcores)
                {
                    if (lc >= next_lcore[port_socket])
                        continue; // 避免重复
                    assigned_lcore = lc;
                    break;
                }
            }

            if (assigned_lcore == RTE_MAX_LCORE)
            {
                throw std::runtime_error("Not enough lcores for port " +
                                         std::to_string(port) +
                                         ", queue " + std::to_string(q));
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
int dpdk()
{
    char *argv[] = {
        "7mm_recv", // name
        "-n", "4",  // Mem_channels
        // "--",
        // "-p","0x3",
        NULL};
    int argc = sizeof(argv) / sizeof(argv[0]) - 1;

    unsigned lcore_id;
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL init\n");

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                                                            NUM_MBUFS * 2, MBUF_CACHE_SIZE, 0, 10240,
                                                            rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    // init rings one subband to one ring
    auto &cfg = GlobalConfig::getInstance();
    // rx_rings.resize(cfg.max_streams, nullptr);
    rx_rings.resize(cfg.max_streams, nullptr);
    for (int i = 0; i < cfg.max_streams; i++)
    {
        char ring_name[32];
        snprintf(ring_name, sizeof(ring_name), "rx_ring_%d", i);
        rx_rings[i] = rte_ring_create(ring_name, RING_SIZE, SOCKET_ID_ANY, 0);
        if (rx_rings[i] == NULL)
        {
            rte_exit(EXIT_FAILURE, "Failed to create ring %s: %s\n", ring_name, rte_strerror(rte_errno));
        }
    }
    // TODO magic number
    uint16_t queues_per_port = 8;
    uint16_t start_dest_port = 60000;
    std::vector<uint16_t> ports = {0, 1};
    // 初始化端口 0
    if (port_init(0, mbuf_pool, queues_per_port) != 0)
        rte_exit(EXIT_FAILURE, " Cannot init port %" PRIu16 "\n", 0);
    // 初始化端口 1
    if (port_init(1, mbuf_pool, queues_per_port) != 0)
        rte_exit(EXIT_FAILURE, " Cannot init port %" PRIu16 "\n", 0);

    // init port config
    auto lcore_params = generate_lcore_params(ports, queues_per_port, start_dest_port);
    int lastcore_id;
    for (int i = 0; i < lcore_params.size(); ++i)
    {
        int subband_index = i / 2; // 每两个队列对应一个 subband
        if (cfg.subbands[subband_index]->enable)
        {
            rte_eal_remote_launch(lcore_recv, &lcore_params[i], lcore_params[i].lcore_id);
            rte_eal_remote_launch(recv2mem, &lcore_params[i], lcore_params[i].lcore_id + lcore_params.size());
        }
        else
        {
            rte_eal_remote_launch(locre_drop, &lcore_params[i], lcore_params[i].lcore_id);
        }
    }
    rte_eal_mp_wait_lcore();
    return 0;
}
