# 处理结果数据格式

本文说明 `observation_mode: 1`（谱线）和 `observation_mode: 2`（连续谱）
产生的网络数据格式。基带模式（`observation_mode: 0`）不使用此格式，而是直接写入
VDIF 帧文件。

## 传输方式和消息边界

处理程序使用 ZeroMQ `PUSH` socket，经 TCP 向 `Storage_node_ip` 和各子带配置的
`port` 发送结果。每个 ZeroMQ message 都是一个完整、独立的“子带－波束－窗口－
积分周期”结果，不进行应用层分片：

```text
+---------------------------+----------------------+----------------------+
| transport header (24 B)   | spectrum_header      | payload              |
| version 1                 | 95 B, version 2      | mode-dependent       |
+---------------------------+----------------------+----------------------+
offset 0                    offset 24              offset 119
```

TCP/ZeroMQ 端口只负责建立连接，不表示子带编号、波束或窗口。多个子带可以共用端口，
Recorder 必须从消息头识别数据身份。当前发送端采用非阻塞发送；Writer 未运行、连接尚未
建立或 ZeroMQ 队列已满时，会整条丢弃该结果，不会阻塞 GPU 处理。因此，TCP 连接内的
字节传输可靠，但本协议不是离线缓存或持久消息队列。

所有多字节字段均按当前 x86-64 主机的原生小端序发送，浮点数采用 IEEE-754。
协议没有进行网络字节序转换；非小端接收机必须显式转换。两个结构均按 1 字节对齐，
不得使用编译器默认填充。

## Transport header（24 字节）

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|---:|---:|---|---|---|
| 0 | 4 | `uint32_t` | `magic` | 固定为 `0x534C5A31`（标识 `SLZ1`） |
| 4 | 2 | `uint16_t` | `version` | 当前为 `1` |
| 6 | 2 | `uint16_t` | `server_id` | 发送服务器编号，当前有效范围 `0–7` |
| 8 | 8 | `uint64_t` | `sequence` | 发送进程内单调递增序号 |
| 16 | 4 | `uint32_t` | `message_bytes` | 后续 body 长度，即 `95 + payload_bytes`，不含本头 |
| 20 | 4 | `uint32_t` | `crc32c` | 后续 body 的 CRC32C |

CRC 使用 CRC-32C/Castagnoli，反射多项式 `0x82F63B78`，初始值
`0xffffffff`，结束时按位取反。校验范围从 `spectrum_header` 第一个字节到 payload
最后一个字节，不包含 transport header。

`sequence` 在每次发送尝试时递增，因此序号缺口可能表示 Writer 不可用、发送队列已满
或链路中断时发送端主动丢弃了完整结果。Recorder 按
`(server_id, subband_id, window_id, beam_id)` 分流后检查顺序。

## `spectrum_header`（95 字节，version 2）

| 偏移 | 长度 | 类型 | 字段 | 单位/含义 |
|---:|---:|---|---|---|
| 0 | 4 | `uint32_t` | `magic` | 固定为 `0x534C5231`（标识 `SLR1`） |
| 4 | 2 | `uint16_t` | `version` | 当前为 `2` |
| 6 | 8 | `uint64_t` | `timestamp_ns` | 本次积分中心的 UTC Unix 时间，ns |
| 14 | 4 | `uint32_t` | `obs_id` | `Observation_ID` 的 CRC32C；手动自动编号时为 `0` |
| 18 | 4 | `uint32_t` | `integration_id` | 该窗口的积分序号，从 `0` 开始递增 |
| 22 | 2 | `uint16_t` | `subband_id` | 全局子带编号 `0–31` |
| 24 | 2 | `uint16_t` | `window_id` | 当前子带内的窗口编号，从 `0` 开始 |
| 26 | 8 | `double` | `subband_start_freq` | 子带起始频率，Hz |
| 34 | 8 | `double` | `subband_end_freq` | 子带截止频率，Hz |
| 42 | 8 | `double` | `start_freq_hz` | payload 第 0 个频率通道的中心频率，Hz |
| 50 | 8 | `double` | `channel_bw_hz` | 相邻频率通道间隔，Hz |
| 58 | 4 | `uint32_t` | `n_channels` | 配置的窗口通道数；连续谱 payload 不按此数量展开 |
| 62 | 1 | `uint8_t` | `stokes` | 谱线结果中每通道的浮点数个数，当前为 `4` |
| 63 | 2 | `uint16_t` | `pkt_id` | 兼容字段；完整 ZeroMQ message 固定为 `0` |
| 65 | 2 | `uint16_t` | `total_pkt` | 兼容字段；无应用层分片，固定为 `1` |
| 67 | 4 | `float` | `exposure` | 配置的积分时间，s |
| 71 | 1 | `uint8_t` | `noise_state` | 噪声源状态：`0=OFF`、`1=ON`、`2=BLANK` |
| 72 | 1 | `uint8_t` | `cal_mode` | `0=普通观测`、`1=噪声源标定观测` |
| 73 | 1 | `uint8_t` | `beam_id` | `0=波束 A`、`1=波束 B` |
| 74 | 1 | `uint8_t` | `reserved2` | 保留，当前为 `0` |
| 75 | 8 | `double` | `ra` | 赤经；当前未配置时为 `0` |
| 83 | 8 | `double` | `dec` | 赤纬；当前未配置时为 `0` |
| 91 | 4 | `uint32_t` | `flags` | 保留标志，当前为 `0` |

由于采用小端序，在线路上两个 magic 的 4 个原始字节分别为 `31 5a 4c 53` 和
`31 52 4c 53`；接收端应按小端 `uint32_t` 比较上述数值，而不是直接比较 ASCII 字节串。

全局子带编号由处理服务器和本机子带共同确定：

```text
subband_id = ServerID * 4 + local_subband_id / 2
```

其中相邻的本机子带配置对应同一频率子带的波束 A/B，所以一台服务器产生 4 个全局
子带编号。`subband_id` 与 ZeroMQ/TCP 端口号没有换算关系。

时间戳是整个积分周期中心对应的 UTC Unix 纳秒数，来源于 VDIF 包头，并已补偿 PFB
群时延和 UTC 闰秒。它不是消息到达 Recorder 的系统时间。

## 谱线 payload（`observation_mode: 1`）

payload 长度必须为：

```text
payload_bytes = n_channels * 4 * sizeof(float)
```

数据按频率通道优先连续排列：

```text
channel 0: float4[0], float4[1], float4[2], float4[3]
channel 1: float4[0], float4[1], float4[2], float4[3]
...
```

设同一通道的两个极化复数 FFT 输出为
`X = Xre + j*Xim`、`Y = Yre + j*Yim`，当前四个 `float` 的实际含义为：

| 分量 | 当前累加量 |
|---:|---|
| `float4[0]` | `Xre² + Xim²` |
| `float4[1]` | `Yre² + Yim²` |
| `float4[2]` | `Xre * Yim` |
| `float4[3]` | `Xim * Yre` |

这些值是积分周期内的累加和，当前不会在发送前除以 FFT 次数或做归一化。虽然字段名
`stokes` 当前为 `4`，payload 仍是上表中的四个原始累加量，并非已经换算好的标准
Stokes `I/Q/U/V`；后续消费者如需 IQUV，必须按系统采用的极化定义和符号约定转换。

第 `k` 个通道的中心频率和窗口高端边界为：

```text
f_center(k) = start_freq_hz + k * channel_bw_hz
f_last_center = start_freq_hz + (n_channels - 1) * channel_bw_hz
f_high_exclusive = start_freq_hz + n_channels * channel_bw_hz
```

## 连续谱 payload（`observation_mode: 2`）

连续谱 message 的 payload 固定为一个小端 IEEE-754 `float`（4 字节），且当前
`window_id` 固定为 `0`。其值为该子带、波束在一个积分周期内所有 FFT 频率通道的
`|X|² + |Y|²` 累加和，同样未除以 FFT 次数或通道数。

连续谱消息仍沿用同一个 `spectrum_header`。其中 `n_channels` 保留配置的窗口通道数
作为元数据，并不表示连续谱 payload 中有这么多个 `float`；接收端必须根据观测模式
将连续谱 payload 严格解释为单个 `float`。

Recorder 应先按消息头的 `subband_id`、`beam_id`、`timestamp_ns` 等身份字段归集来自
各服务器的独立结果，再执行连续谱组合；不能根据连接端口或消息到达顺序推断子带。

## 接收端校验要求

建议按以下顺序拒绝无效消息：

1. ZeroMQ message 至少为 `24 + 95` 字节，并且总长度等于
   `24 + transport.message_bytes`。
2. transport `magic/version`、`server_id` 和 body CRC32C 正确。
3. spectrum `magic/version` 正确，且 `pkt_id == 0`、`total_pkt == 1`。
4. `subband_id <= 31`、`beam_id <= 1`、`n_channels > 0`，并验证
   `subband_id / 4 == server_id`。
5. 谱线 payload 恰好为 `n_channels * 16` 字节；连续谱 payload 恰好为 4 字节。
6. 用身份字段分流后检查 `sequence`、`integration_id` 和 `timestamp_ns`，不要用端口号
   或到达先后覆盖其他服务器、子带、波束或窗口的数据。

## Python 解码布局示例

下面只展示结构拆包；生产接收程序仍必须执行长度、版本和 CRC 校验：

```python
import struct

TRANSPORT = struct.Struct("<IHHQII")
SPECTRUM = struct.Struct("<IHQIIHHddddIBHHfBBBBddI")

assert TRANSPORT.size == 24
assert SPECTRUM.size == 95

transport = TRANSPORT.unpack_from(message, 0)
header = SPECTRUM.unpack_from(message, TRANSPORT.size)
payload_offset = TRANSPORT.size + SPECTRUM.size  # 119
```

协议的权威定义位于
[`include/SpectrumTransport.hpp`](../include/SpectrumTransport.hpp) 和
[`include/SpectrumSender.hpp`](../include/SpectrumSender.hpp)。修改字段类型、顺序或
长度时，必须同步升级协议版本并同时更新处理程序与 Recorder；不能只修改一端。
