# agent_approver

用 **M5StickS3** 当 AI agent 的"实体审批 + 状态屏"。

- agent (先支持 **Cursor**，预留 claude-code / codex) 通过 **hook** 把"想执行的危险操作"推到 StickS3，你在 StickS3 上按 **A 批准 / B 拒绝**，结果实时回传给 agent。
- 同时 hook 把 agent "正在忙什么"（编辑文件 / 跑命令 / 收到任务 / 完成）也推到 StickS3，扫一眼就知道它在干嘛。

```
Cursor / Claude / Codex
   │  hook (beforeShellExecution / afterFileEdit / stop / ...)
   ▼
hook.py  ──HTTP 127.0.0.1:8799──▶  relay.py (Mac 常驻)
                                       │  待审批队列 + 最新活动
                                       ▲
                                       │  HTTP LAN long-poll
                                  M5StickS3 (WiFi)
                                    GET  /stick/poll   拉待办
                                    POST /stick/decide A=allow / B=deny
```

为什么中间要一个 relay：hook 打 `127.0.0.1` 又快又稳、不用知道 StickS3 的 IP；StickS3 只往外连（跟现有 stick_s3_eyes 的 bridge 一样）；StickS3 离线时 relay 能立刻让 hook 回退到 Cursor 原生审批，**绝不会把 agent 卡死**。

## 目录

```
agent_approver/
├── relay/
│   ├── relay.py                     # Mac 上的中枢 HTTP server (stdlib, 免装依赖)
│   └── com.agentapprover.relay.plist# launchd 开机自启模板
├── hooks/
│   ├── hook.py                      # Cursor hook 入口 (一脚本多模式)
│   ├── hooks.template.json          # 写进 ~/.cursor/hooks.json 的模板
│   └── config.example.json          # ~/.config/agent_approver/config.json 示例
├── firmware/                        # StickS3 Arduino 工程 (M5Unified)
│   ├── firmware.ino  net.*  web_config.*  wifi_store.*  app_prefs.*
│   ├── partitions.csv  flash.sh
└── install.sh                       # 一键安装 hooks + 默认配置 (可选 launchd)
```

## 快速开始

### 1. 启动 relay
```bash
python3 agent_approver/relay/relay.py
```
启动日志里会打印：
```
StickS3 relay URL -> http://192.168.31.114:8799   # 记下这个地址
status page       -> http://127.0.0.1:8799/        # 浏览器可看实时状态
```
想开机自启：`agent_approver/install.sh --launchd`。

### 2. 装 hooks
```bash
agent_approver/install.sh
```
它会把 hook 合并进 `~/.cursor/hooks.json`（保留你已有的 hooks，可重复运行），并写一份默认配置到 `~/.config/agent_approver/config.json`。**装完重启 Cursor** 生效（设置 → Hooks 能看到）。

### 3. 烧固件 + 配网
```bash
# StickS3 长按侧面键 ~2s 进下载模式
agent_approver/firmware/flash.sh
```
首次开机没 WiFi → 自动开热点 `Approver-XXXX`，手机/电脑连上后浏览器打开 `http://192.168.4.1`：
1. 添加你的 WiFi；
2. 把第 1 步打印的 **relay URL** 填进 "Relay 地址" 保存。

连上 WiFi 后，StickS3 也能用 `http://<它的IP>/` 进同一个配置页。**长按 B** 可随时强制开/关配网热点。

## 用法

- **危险命令**（`rm -rf` / `sudo` / `git push --force` / `dd` / `mkfs` …）→ StickS3 亮屏显示命令，**A 批准 / B 拒绝**。
- **普通操作** → 直接放行，只在 StickS3 上更新"正在忙什么"。
- **StickS3 离线 / relay 没起 / 超时** → 回退到 Cursor 原生审批（`ask`），不阻塞。

### StickS3 按键
| 场景 | A | B |
|---|---|---|
| 有待审批 | 批准 | 拒绝 |
| 空闲 | 切屏幕亮度 | （长按）开/关配网 |

## 配置 `~/.config/agent_approver/config.json`

```json
{
  "url": "http://127.0.0.1:8799",
  "approval_timeout_ms": 150000,
  "shell_mode": "risky",   // risky=只拦危险命令 | all=所有命令都要批 | off=不拦
  "mcp_mode": "off",       // all=所有 MCP 调用都要批 | off=只上报
  "fallback": "ask",       // 不可用时: ask(交回 Cursor) | allow | deny
  "activity": true,
  "risky_patterns": ["\\bterraform\\s+apply\\b"]   // 追加自定义危险正则
}
```

危险命令的判定见 `hooks/hook.py` 里的 `BUILTIN_RISKY`，`risky_patterns` 会在其基础上追加。

## 支持其它 agent（claude-code / codex）

relay 的接口是 agent 无关的（`agent` 只是个字段）。接新 agent 只要让它的 hook 机制调到同样的 HTTP：
- 批准：`POST /hook/approval {agent,tool,title,detail,cwd,timeout_ms}` → `{decision}`
- 活动：`POST /hook/activity {agent,kind,text,cwd}`

`hook.py` 已经能复用：给对应 agent 设环境变量 `AGENT_APPROVER_AGENT=claude`（屏幕上会显示来源），再按各 agent 的 hook 文档把事件接到 `hook.py shell|mcp|activity ...` 即可。Cursor 已经接好。

## 卸载
```bash
agent_approver/install.sh --uninstall
```
（移除写进 `~/.cursor/hooks.json` 的条目 + launchd；保留你的配置文件。）
