# StackChan 融合方案

## MicroDuck 生产版本

本版本在 Fusion 版本已验证的云链路、MQTT 播报与桌面协同基础上，引入 MicroDuck 的核心思路：小步拆分、明确资源所有权、网络与外设解耦，以及面向嵌入式实时性的最小接口设计。

实现内容包括 Push MQTT 两阶段生命周期、30 秒心跳、QoS1 与 ACK 闭环、μ-law 音频播报、屏幕确认、传感器/电源管理、Cat 表情、动作联动、System Tray 与 Desktop Widget；各模块按 MicroDuck 思路渐进抽取，保持接口清晰和可验证。

生产固件：`xiaozhi.bin`，App-Only 写入 `0x410000`，SHA-256：`0A05C4D4B1403760EB4CF0CBB1B4CAFDF314E6A8067574BAADD50EA78C50020A`。

### 致谢

感谢 Fusion 版本阶段的设计、联调与测试积累；感谢 MicroDuck 项目及其贡献者提供的模块化嵌入式架构启发；感谢 M5Stack StackChan、Xiaozhi.me、ESP-IDF、EdgeTTS、Paho MQTT 及相关开源社区的项目与维护者。
## 架构

```
机器人 (M5Stack CoreS3, 固件 v1.2-mqttpush, 唤醒词「阿松」)
   │ 语音 (ASR/LLM/TTS 在 xiaozhi.me 云端)
   ▼
xiaozhi.me 云智能体 (STACK, 提示词见 prompt-阿松-v3.md)
   │ MCP (wss://api.xiaozhi.me/mcp)
   ▼
xiaozhi-mcp 云桥接 (mcp_pipe.py + server.py, 本机)
   │ agent_status / agent_query / agent_pending / agent_confirm / agent_result_check / robot_snap ...
   ▼
融合网关 fusion_gateway.py (:8010, Bearer 认证)
   │
   ├── 播报链路: 单 Worker FIFO → EdgeTTS(粤语) → µ-law → EMQX MQTT
   │            (stackchan/{mac}/push, START 带 msg_uid, 固件 ACK 后点杀)
   ├── 控制链路: 表情/头部/动作/LED → EMQX MQTT
   │            (stackchan/{mac}/cmd, v1 JSON, 过期校验, ACK/完成回执)
   ├── codex   (Stop hook 带 msg_uid → 网关幂等入队)
   ├── claude  (Stop/SessionEnd/PermissionRequest hook + confirm_mcp 确认回环)
   ├── agy     (Antigravity fusion hooks)
   ├── pi      (扩展 hooks-bridge.ts)
   └── vscode  (vscode_hook.py, VS Code 任务完成上报)
        └── 机器人任务 → agent 自己的可见窗口执行 (Codex-Asong / ClaudeCode-Asong / ...)
```

两条链路：

| 链路 | 说明 |
|---|---|
| 云链路（主） | 机器人语音走 xiaozhi.me；agent 事件经网关 → EMQX MQTT µ-law 主动播报 |
| 自建链路 | **已停用**（容器 Exited；播报不再依赖 xiaozhi-esp32-server） |

## 功能

| 能力 | 说明 |
|---|---|
| 主动播报 | agent 完成/出错/需确认 → 网关立即推送（≤60 字 LLM 口语化摘要，长文本 LLM 提炼、失败降级截断），msg_uid + ACK 点杀不重不漏 |
| 唤醒补播 | 机器人离线时消息保留 pending 队列，唤醒后 `agent_pending` 补播 |
| 状态查询 | 「检查 XX 状态」→ `agent_status`（4 个 agent 可用性/进程/最近事件，<5s） |
| 任务执行 | 「让 XX 做…」→ `agent_query`，在 agent 自己的可见窗口执行，结果回流播报 |
| 确认回环 | claude 权限请求 → 机器人念问题 → 语音回答 → 回写 allow/deny（claude 完整支持） |
| 设备控制 | 点头/摇头/转向/表情/拍照/LED（固件自动跟随状态灯） |

控制命令与音频严格分题：`/push` 只承载 `[START][µ-law batch][STOP]`，`/cmd` 只承载短小的
版本化 JSON。这样控制指令不会插入音频 FIFO；设备端对头部角度、移动时长和灯光 RGB 再做一次边界校验。

## 快速开始

### 新电脑 / 新机器人

完整部署步骤（含全部占位符配置、刷固件、配网、xiaozhi.me 绑定、四 agent hooks）见 **[DEPLOY.md](DEPLOY.md)**。

### 本机服务

```powershell
# 融合网关 (:8010, 必须)
powershell -ExecutionPolicy Bypass -File gateway\run_gateway.ps1
# 云桥接 (机器人走 xiaozhi.me 时, 必须)
powershell -ExecutionPolicy Bypass -File xiaozhi-mcp\run_bridge.ps1
# 备用链路容器 (可选)
docker compose -f server\docker-compose.fusion.yml up -d
# 托盘 + 自启 (可选)
powershell -ExecutionPolicy Bypass -File gateway\install_autostart.ps1
```

### 验证

```powershell
python scripts\verify_connectivity.py
```

全部 PASS 后：对机器人说「阿松」唤醒 → 应自动播报待办；说「检查 agent 状态」→ 播报四 agent；
说「让 codex 总结项目」→ 桌面弹出 Codex 窗口执行 → 完成后唤醒机器人听结果。

## 四 Agent 接入

| Agent | 接入方式 | 主动上报 | 语音回写确认 |
|---|---|---|---|
| codex | `~/.codex/hooks.json` → `agents/codex_hook.py`；`config.toml` `bypass_hook_trust=true`、`[windows] sandbox='unelevated'` | ✅ 桌面+CLI | ❌（在 codex 界面确认） |
| claude | `~/.claude/settings.json` hooks → `agents/claude_hook.py`（v08.09 起主存 settings.json——Windows 2.1.x 的 settings.local.json 有 #64699 静默失效 BUG；ccswitch 覆盖后托盘「安装/修复 Claude Hooks」一键自愈）；可见窗口经 `agents/claude_visible_run.py` 上报完成；`agents/confirm_mcp.py` | ✅ | ✅ 完整回环 |
| agy / Antigravity | `~/.gemini/config/hooks.json` `stackchan` 段 → `agents/antigravity_hook.py` | ✅ CLI 归属 agent=agy | ❌ |
| pi | `~/.pi/agent/extensions/hooks-bridge.ts` → 网关 | ✅ | ❌ |
| vscode | `agents/vscode_hook.py`，任务/终端结束上报 done；`AGENT_CLIS` 已注册；语音派发**已拒发**（防 `code -r` 误开文件） | ✅ | ❌ |

任务执行方式：`agent_query` 打开 agent 自己的可见控制台窗口（标题 `Codex-Asong` /
`ClaudeCode-Asong` / `Antigravity-Asong` / `pi-Asong`，脚本存于 `gateway/state/visible_runs/`），
结果经各 agent hooks 写入网关，机器人唤醒后播报。

> 所有 hook 上报均携带 **msg_uid**（`{agent}_{session8}_{hash(最后一条 assistant 消息)}`），
> 网关按 msg_uid 幂等（重复上报静默 200），固件对 START 回发 ACK，网关收到 ACK 后物理删除
> pending 记录——同一轮任务绝不重复播报，离线消息保留兜底自动重试。

## 机器人固件

- 当前：**v1.2-mqttpush**（`firmware/post-fw-v1.2-mqttpush/`，构建脚本 `build_fw_v112.ps1`）
- 基座：07.31 已跑通的 `reference/stackchan-xiaozhi-firmware`（heavenchenggong 系，
  含「阿松」+ LED 补丁；**不要用 HtSz 主分支**——有 bug 起不来）
- 第二条 MQTT 链路（`stackchan/{mac}/push`）：订阅 EMQX 推送主题，µ-law 直出播放（绕开
  Opus 解码器兼容问题），START 解析 msg_uid → 回发 ACK；SSID 智能路由（EMQX 首选，
  AP 隔离时自动降级）；MQTT buffer 8KB、poll 读超时 5s、**keepalive 15s**；
  lwIP TCP 收窗口 16KB（µ-law 16KB/s 有余量）；播报期间关 WiFi 节能，播完恢复；
  打断后待播放队列排空自然切回待机。
- **Phase 8.1 动作联动**：收到 `done/error` → 点头；`question` → 歪头 +15°；
  待机闲逛摆头 20s 一次。
- **v08.08 LED 根治**：PY32 GPIO13 出厂序列 + `led_manual_` 待机锁 + I2C 互斥，
  真机播报绿→待机暖橙；固件已重建（14:31 产物）。
- **Phase 8.2 拍照**：`robot_snap` → 固件拍 JPEG 分块（`stackchan/{mac}/photo`, QoS1）
  → 网关重组校验。
- v1.0.6/1.0.5/1.0.4 历史版本见下方版本记录。
- 升级：app-only 刷 `xiaozhi.bin @ 0x410000`，保留配置；构建 espressif/idf:v5.5.2
  （5.5.4 会黑屏）。

## 服务与运维

| 服务 | 端口 | 说明 |
|---|---|---|
| 融合网关 | 8010 | 18 个 MCP 工具（含 robot_snap / robot_face / robot_head / robot_gesture / robot_led / robot_servo_calibrate），Bearer 认证；单 Worker 推送 FIFO + msg_uid 幂等 |
| xiaozhi-mcp 云桥接 | — | mcp_pipe.py + server.py，心跳 60s |
| EMQX 公共 broker | 1883 | 音频 `/push` 与控制 `/cmd` 分题（broker-cn.emqx.io，QoS1 + 固件 ACK/完成回执） |
| Codex↔机器人桥接 | — | `bridge/stackchan_mcp.js`（MCP stdio：check_task / respond） |
| xiaozhi-esp32-server (Docker) | — | **已停用**（云链路不依赖） |
| 系统托盘 | — | 状态监视 + 网关守护（单实例保护）+ 队列操作菜单（查看/清空）+「安装/修复 Claude Hooks」自愈菜单 |

守护与计划任务全部经 `wscript.exe` + VBS 隐藏启动（无弹窗），`install_autostart.ps1` 一键注册。

## 故障排查

| 症状 | 处理 |
|---|---|
| 「检查 agent 状态」超时 | 网关/桥接未启动；探测已缓存 120s + 并发（<5s） |
| codex 窗口报 Access denied | `~/.codex/config.toml` `[windows] sandbox='unelevated'`；不要加 `--sandbox workspace-write` |
| 中文任务乱码 | hook 脚本读 UTF-8；`mcp_pipe` 子进程 `PYTHONUTF8=1`（已修复，重启 codex 桌面生效） |
| 机器人念陈旧结果 | `agent_result_check` 只返回 30 分钟内新结果（已修复） |
| 托盘两个图标 | `fusion_tray.ps1` 单实例保护（已修复） |
| 机器人不播报 | 网关日志看 `push ack`/`push no-ack`：ack=已送达；no-ack=机器人 push MQTT 离线，
  消息保留 pending 兜底自动重试；config.json 损坏会 Fail-Fast 拒启 |
| Claude 任务不播报 / hooks 丢失 | 检查 `~/.claude/settings.json` 是否含四钩子（v08.09 起主存
  settings.json：Windows 2.1.x 的 settings.local.json 有 #64699 静默失效 BUG，升级后 hooks
  会彻底不触发）；丢失时右键托盘「安装/修复 Claude Hooks」或重跑
  `agents/install_claude_hooks.ps1` 自愈 |
| 对机器人说「让 vscode 做…」 | 网关返回 "VS Code 暂不支持语音派发任务"——请手动在
  VS Code 运行任务，结束经 `vscode_hook.py` 自动播报（防 `code -r` 误开文件） |
| 播报卡顿/无声 | 确认固件 v1.2-mqttpush（µ-law + 16KB 窗口）；网关日志 `push ok` 后无 ack
  说明机器人 push MQTT 掉线（keepalive 15s 已加固） |

## 已知边界

- 云链路播报为**非打断式主动推送**（EMQX MQTT），消息带 msg_uid + ACK 闭环；
  机器人离线时消息保留 pending，唤醒后 `agent_pending` 补播。
- Codex / Antigravity 桌面应用与 VS Code 插件面板的**内部会话无法外部注入**；
  机器人任务在对应 CLI 窗口执行，插件会话仍经 hooks 上报事件。
- 确认回环仅 claude 完整（`--permission-prompt-tool` + `confirm_mcp`）；
  codex/agy/pi 只上报「需要确认」，回写需在 agent 界面完成。
- 语音端到端延迟约 1.5–2.5s（云端 ASR/LLM/TTS 所致），非打断式播报可接受。
- VS Code 语音派发不支持（已拒发防退化）；拍照依赖机器人联网且 push MQTT 在线。

## 敏感信息

本仓库**不含任何真实凭据**：token / API key / MAC / 域名均为占位符
（`YOUR_*` / `AA:BB:CC:DD:EE:FF`）。真实值只存在于本机 `.env`、`config.json`、
docker 配置。`.gitignore` 已忽略所有运行时敏感文件。
部署时按 [DEPLOY.md](DEPLOY.md) 第 4 节逐项替换。



