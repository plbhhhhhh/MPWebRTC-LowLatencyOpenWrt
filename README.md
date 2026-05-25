# MPWebRTC-LowLatencyOpenWrt

本仓库整理了大创项目中的两个实时视频低延迟传输方案：

- `MPWebRTC/`：基于 WebRTC 的多路径视频会议框架及 QoE 感知调度。
- `LowLatencyOpenWrt/`：基于 OpenWrt Wi-Fi AP 的实时视频流帧级低延迟调度机制。

两个方案面向的问题不同：`MPWebRTC` 从端侧 WebRTC 发送链路出发，利用多网卡和多路径传输提升复杂网络下的视频会议稳定性；`LowLatencyOpenWrt` 从 AP 侧 Wi-Fi 下行队列出发，在不修改终端和应用的前提下降低实时视频流的尾延迟与卡顿概率。

## 目录结构

```text
.
├── MPWebRTC/
│   └── src/                         # WebRTC 修改文件镜像和实验辅助文件
│       ├── p2p/base/                 # ICE 候选路径管理与双路径发送
│       ├── rtc_base/                 # 多路径模式、路径速率和 quota hint 状态
│       ├── modules/pacing/           # pacer 发送前路径提示和帧亲和调度
│       ├── modules/rtp_rtcp/         # RTP 包优先级、path hint 透传、FEC/RTX 统计
│       ├── modules/congestion_controller/
│       │   └── ...                   # GCC 分路径反馈和探测速率联动
│       ├── call/、video/              # 视频发送、接收缓冲和 NACK 相关改动
│       └── examples/                 # peerconnection / Janus 实验入口
└── LowLatencyOpenWrt/
    ├── package/kernel/mac80211/patches/subsys/
    │   ├── 354-mac80211-add-video-flow-isolation-and-priority-scheduling.patch
    │   ├── 355-mac80211-add-video-frame-boundary-perception.patch
    │   └── 356-mac80211-add-video-frame-latency-risk-estimator.patch
    └── package/kernel/mt76/patches/
        └── 900-mt76-add-video-low-layer-latency-feedback.patch
```

其中 `MPWebRTC/src/readme.md` 还保留了 WebRTC 修改镜像的更细粒度说明。当前 `MPWebRTC/src/` 主要用于查看和打包相对原始 WebRTC 的改动，不等价于一份完整可直接构建的上游 WebRTC 源码树。

## 方案一：MPWebRTC 多路径视频会议框架

`MPWebRTC` 在 WebRTC 发送端引入多路径感知能力，使视频会议可以同时利用两条可用链路，并根据路径状态和媒体包重要性进行差异化调度。

核心模块如下：

- 路径评价与保活：扩展 ICE 连接管理，除最优候选路径外保留次优路径，并保持两条路径处于可发送状态。路径排序综合连接状态、远端提名、最近收包、网络成本、ICE 优先级和 RTT 等信息。
- 优先级识别打标：在 RTP 明文发送阶段为包增加优先级信息，当前代码中的优先级顺序为 `重传包 > 音频包 > 关键帧/H.264 SPS/PPS > FEC/RED/填充冗余包 > 普通媒体包`。
- 多路径调度：扩展 GCC、pacer、RTP egress 和 ICE 发送链路，使用分路径反馈估计主/次路径可用速率。高优先级包优先走当前更优路径，普通媒体包按估计速率比例分配，并尽量保持同一视频帧走同一路径以减少乱序。
- 实验模式切换：通过 `--webrtc_send_stack` 在运行时切换发送栈，便于和原生 WebRTC、朴素轮询方案对比。

支持的发送模式：

| 模式 | 含义 |
| --- | --- |
| `native` / `stock` | 原生 WebRTC 行为，不启用多路径发送和路径提示 |
| `extended` | 默认实验模式，启用双路径 ICE 发送、GCC 速率比例分配和高优先级包择优 |
| `round_robin` / `rr` | 主路径/次路径轮询发送，用作朴素多路径对照 |
| `single_path_pacer` | ICE 仍单路径发送，但保留 pacer 侧路径提示和优先级排序逻辑 |

主要代码入口：

- `MPWebRTC/src/rtc_base/network_route_probe_bitrate_tracker.{h,cc}`：多路径模式、分路径探测速率、GCC delay-based 速率和 `MultipathQuotaHint`。
- `MPWebRTC/src/p2p/base/p2p_transport_channel.cc`：主/次 ICE 连接发送、调度窗口、分路径 quota 和 `path_hint` 落地。
- `MPWebRTC/src/modules/pacing/task_queue_paced_sender.cc`：发送前为 RTP 包分配路径提示，并缓存同一帧的路径选择。
- `MPWebRTC/src/modules/rtp_rtcp/source/rtp_sender_egress.cc`：确定 RTP 包优先级并将 `packet_priority`、`path_hint` 传给底层传输。
- `MPWebRTC/src/modules/congestion_controller/`：分路径反馈适配和 GCC 探测联动。
- `MPWebRTC/src/examples/peerconnection/client/`：命令行模式参数入口。

实验平台使用 MahiMahi/mpshell 构造双路径环境，对比 `native`、`extended` 和 `round_robin`。典型场景包括链路容量受限、容量异构和时延异构；评价指标包括吞吐量、平均 QP 和卡顿时间。

## 方案二：OpenWrt AP 侧低延迟视频调度

`LowLatencyOpenWrt` 在 OpenWrt 的 Linux `mac80211` 子系统和 `mt76` 驱动中实现 AP 侧下行调度优化。方案不读取视频载荷、不修改终端或服务器，而是基于 AP 可见的端口、包长、到达时间、队列状态和驱动反馈来识别视频帧并估计延迟风险。

四个主要补丁如下：

| 补丁 | 作用 |
| --- | --- |
| `354-mac80211-add-video-flow-isolation-and-priority-scheduling.patch` | 在 FQ-CoDel 旧流阶段新增 `video_flows` 队列，将配置端口的视频流与普通 `old_flows` 分离；视频流获得更大的 DRR quantum，并在旧流阶段按加权方式优先调度 |
| `355-mac80211-add-video-frame-boundary-perception.patch` | 对视频流进行帧边界感知；支持 AVG 模式包长稳定区间变化检测、LAST 模式大包序列后小尾包检测，并提供包数/时间阈值降级边界 |
| `356-mac80211-add-video-frame-latency-risk-estimator.patch` | 增加帧延迟风险估计和帧级保护；综合 Pre-AP 相对延迟、FQ 队列积压代理值和驱动低层延迟 EWMA，超过阈值时允许当前高风险帧在赤字不足时继续完成发送 |
| `900-mt76-add-video-low-layer-latency-feedback.patch` | 在 mt76 PCIe 发送路径记录驱动可见的低层发送延迟，并通过 `ieee80211_video_low_layer_latency_report()` 上报给 mac80211；该补丁只提供测量反馈，不改变 mt76 调度逻辑 |

AP 侧调度流程：

1. 视频流进入独立的 `video_flows` 队列，和普通背景旧流分离。
2. 入队时利用包长序列识别视频帧边界，并维护帧编号、帧大小、帧完成时间等状态。
3. 出队时估计当前帧延迟风险；若超过阈值，则触发帧级保护，优先完成该帧剩余包的发送。
4. mt76 驱动反馈 `driver_queue`、`dma_to_txfree` 和总低层延迟样本，用于补偿 mac80211 无法直接观察的低层排队和发送过程。

常用 debugfs 配置和观测项：

```sh
# 确保 debugfs 已挂载
mount -t debugfs none /sys/kernel/debug

# 查找 PHY，例如 phy0
ls /sys/kernel/debug/ieee80211/


# 固定视频帧周期，单位 us；30 FPS 可设为 33333。
# 写 0 表示使用自动估计模式。
echo 33333 > /sys/kernel/debug/ieee80211/phy0/video_frame_interval_us

# 设置高延迟保护阈值，单位 us；默认 50000。
echo 50000 > /sys/kernel/debug/ieee80211/phy0/video_latency_high_us

# 查看帧边界、延迟估计和帧级保护统计
cat /sys/kernel/debug/ieee80211/phy0/video_frame_stats

# 查看 mt76 低层发送延迟样本
cat /sys/kernel/debug/ieee80211/phy0/mt76/low-layer-latency
```

实验平台使用一台 OpenWrt AP、一台有线发送端、一台无线视频接收端和一台无线背景流客户端。视频流使用 FFmpeg 生成 H.264 RTP/UDP 流，背景流使用 iPerf3 TCP 流量。实验按轻度、中度、重度无线负载评估平均帧延迟、P99 帧延迟、卡顿率和背景流吞吐量。

## 构建与使用提示

### WebRTC 侧

`MPWebRTC/src/` 当前是 WebRTC 改动文件镜像。若需要构建，需要先准备一份匹配版本的完整 WebRTC 源码树，再将本目录中的改动覆盖到对应路径，随后按 WebRTC 原生 `gn` / `ninja` 流程构建示例程序。

运行 peerconnection 示例时，可通过如下参数选择实验栈：

```sh
--webrtc_send_stack=native
--webrtc_send_stack=extended
--webrtc_send_stack=round_robin
--webrtc_send_stack=single_path_pacer
```

### OpenWrt 侧

`LowLatencyOpenWrt/` 是带有实验补丁的 OpenWrt 源码树，可按 OpenWrt 标准流程配置和编译：

```sh
cd LowLatencyOpenWrt
./scripts/feeds update -a
./scripts/feeds install -a
make menuconfig
make -j"$(nproc)"
```

实验补丁主要作用于 `mac80211` 和 `mt76`。若迁移到其他 OpenWrt 版本或其他无线驱动，需要重新检查补丁上下文、debugfs 路径和驱动 TX 完成反馈路径。

## 实验结论概述

- WebRTC 多路径方案在单路径带宽不足时能够聚合双路带宽，在容量异构时可避免向劣质链路过度分配，并通过关键媒体包优先走优质路径降低乱序、重传和卡顿风险。
- 朴素轮询多路径虽然能使用多条链路，但在异构时延或异构容量下容易产生乱序、伪丢包、无效重传和 FEC 冗余膨胀。
- OpenWrt AP 侧方案在轻负载下对背景流影响较小，主要改善尾延迟；在中高负载下能明显降低 P99 帧延迟和卡顿率。
- AP 侧方案在重负载下会牺牲较多背景流吞吐量，后续可加入视频流赤字偿还或更严格的公平性约束，减少对背景业务的长期挤占。


