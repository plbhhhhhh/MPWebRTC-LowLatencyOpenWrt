# WebRTC 多路径扩展变更说明

本目录（`src/`）是当前工作区相对原始 WebRTC 的**变更镜像**：仅包含“已修改/新增”文件，并保持原目录结构。  
这些改动围绕一个目标：在 WebRTC 发送链路中引入可切换的多路径能力（ICE 多路径发送 + 发送前调度提示 + 拥塞控制联动），用于提升复杂网络环境下的吞吐稳定性和抗抖动能力。

## 1. 项目目标与设计思路

本项目在尽量复用 WebRTC 原有模块边界的前提下，新增了“多路径感知”的控制面与数据面协同：

- 控制面：维护路径级估计信息（delay-based/probe bitrate）、配额提示（quota hint）与模式开关。
- 数据面：在包发送前后（pacer / RTP egress / ICE 发送）引入路径选择与状态透传。
- 兼容性：保留接近原生行为的模式（`native`），并通过 runtime flag 切换到实验栈。

## 2. 发送栈模式（runtime 可切换）

核心枚举定义在 `rtc_base/network_route_probe_bitrate_tracker.h`，通过 `--webrtc_send_stack` 设置：

- `native` / `stock`
  - 原生 WebRTC 行为，不启用本扩展的多路径发送与路径提示逻辑。
- `extended`（默认）
  - 启用双路径 ICE 发送、基于估计速率的配额提示，以及优先路径转发。
- `round_robin` / `rr`
  - 发送包在主/次路径间交替分配，便于做路径公平性或对照实验。
- `single_path_pacer`
  - ICE 层保持单路径发送，但继续发布多路径提示，让 pre-pacer 优先级排序/窗口机制可继续工作。

模式解析入口：

- `examples/peerconnection/client/flag_defs.h`
- `examples/peerconnection/client/apply_webrtc_stack_from_flags.h`
- `examples/peerconnection/client/apply_webrtc_stack_from_flags.cc`

## 3. 关键改动模块与职责

### A. 路径状态与全局多路径控制（`rtc_base/`）

- 新增 `network_route_probe_bitrate_tracker.{h,cc}`。
- 作用：
  - 维护每个 `network_id` 的 delay-based/probe 估计速率。
  - 管理多路径配额提示 `MultipathQuotaHint`（主次路径估计 + 调度窗口）。
  - 提供实验模式开关与查询接口，作为跨模块共享状态的轻量桥接层。

### B. ICE 传输与路径调度（`p2p/base/`）

- 重点文件：`p2p_transport_channel.cc/.h`、`basic_ice_controller.*` 等。
- 作用：
  - 增强候选路径管理与活跃连接选择，使发送侧可感知主/次路径。
  - 在多路径模式下引入路径调度状态（含窗口/轮转相关状态）。
  - 对外暴露/更新 `NetworkRoute`，供上层反馈与拥塞控制链路关联。

### C. 拥塞控制与反馈适配（`modules/congestion_controller/`）

- 重点文件：
  - `goog_cc/goog_cc_network_control.*`
  - `goog_cc/probe_controller.*`
  - `rtp/transport_feedback_adapter.*`
- 作用：
  - 扩展网络控制器与探测逻辑，使路径变化可触发更合理的 probe 与速率更新。
  - 将发送包与 `NetworkRoute` 关联，做分路径在途数据统计与反馈归集。
  - 为多路径调度提供更可信的带宽信号输入。

### D. 发送前调度与出站发送链路（`modules/pacing/` + `modules/rtp_rtcp/` + `call/`）

- 重点文件：
  - `modules/pacing/task_queue_paced_sender.*`
  - `modules/rtp_rtcp/source/rtp_sender_egress.*`
  - `call/rtp_video_sender.*`
- 作用：
  - pacer 读取多路径配额提示，影响包出队与优先级决策。
  - RTP egress 扩展发送上下文，承接路径相关信息并下传。
  - 视频发送侧与 RTP 模块接口打通，保证路径提示与发送统计链路连续。

### E. 接收鲁棒性与观测补充（`video/` + `modules/video_coding/`）

- 重点文件：`video_receive_stream2.*`、`rtp_video_stream_receiver2.cc`、`nack_requester.*` 等。
- 作用：
  - 调整接收侧缓冲/NACK 处理细节，提高在乱序、抖动、丢包场景下的稳定性。
  - 补充单测，降低多路径发送策略迭代时的回归风险。

### F. 示例与实验入口（`examples/`）

- 重点文件：
  - `examples/peerconnection/client/*`
  - `examples/BUILD.gn`
- 作用：
  - 提供命令行切换发送栈模式的入口。
  - 便于快速搭建实验，对比 `native` / `extended` / `round_robin` / `single_path_pacer` 行为差异。

## 4. 端到端工作流（高层）

1. 应用层通过 `--webrtc_send_stack` 选择运行模式。  
2. ICE 层维护主/次路径状态并产出 `NetworkRoute`。  
3. 发送侧反馈进入 GCC，更新路径级估计与探测结果。  
4. `rtc_base` 汇总并发布路径提示（quota/probe/delay-based）。  
5. pacer 与 RTP 发送链路据此执行发送优先级和路径分配。  
6. 接收侧通过 NACK/缓冲策略改动提升对网络波动的容忍度。

## 5. 如何使用本镜像目录

- 本目录仅用于“查看和打包变更”，不替代完整源码树。
- 复制规则：
  - 包含：已跟踪文件的修改（含 staged/unstaged）+ 未跟踪新增文件。
  - 不含：删除文件（因为源文件已不存在，无法镜像复制）。

## 6. 快速验证建议

- 基线对照：`--webrtc_send_stack=native`
- 实验主线：`--webrtc_send_stack=extended`
- 调度对照：`--webrtc_send_stack=round_robin`
- 单路径调度验证：`--webrtc_send_stack=single_path_pacer`

建议至少采集以下指标做横向对比：端到端吞吐、帧率稳定性、卡顿/冻结比率、重传率、时延分布与路径切换时恢复时间。
