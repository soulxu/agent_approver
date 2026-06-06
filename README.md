# agent_approver

用 **M5StickS3** 当 AI agent 的“实体审批 + 状态屏”，通过 **蓝牙 (BLE)** 直连 Mac。

- agent (先支持 **Cursor**，预留 claude-code / codex) 通过 **hook** 把“想执行的危险操作”推到 StickS3，你在 StickS3 上按 **A 批准 / B 拒绝**，结果实时回传给 agent。
- 同时**所有 Cursor hook**（收到任务 / 思考 / 读文件 / 编辑 / 跑命令 / MCP / 子任务 / 压缩 / 完成…）都会把 agent “正在忙什么”推到 StickS3。
- **多个 agent 同时跑**时，StickS3 默认显示**总览**（每个对话一行各自的状态），按键可钻进**某一个 agent 看详情**。agent 用 Cursor 的 `conversation_id` 区分，标签取工程目录名。

```
Cursor 对话A / 对话B / ...          (每个对话 = 一个 agent)
   │  全部 hook (sessionStart / beforeShell / afterFileEdit / stop / ...)
   ▼
hook.py ──明文 HTTP 127.0.0.1:8799──▶ relay.py (Mac 常驻)
                                         │  待审批队列 + 各 agent 当前状态
                                         ▲  BLE (Nordic UART Service)
                                         │  relay 当 central 主动连过去
                                    M5StickS3 (BLE 外设 "AgentApprover")
```

为什么中间要一个 relay：hook 打 `127.0.0.1` 又快又稳；relay 统一管蓝牙连接、待审批队列、超时回退；StickS3 没连上蓝牙时 relay 能立刻让 hook 回退到 Cursor 原生审批，**绝不会把 agent 卡死**。

**两段链路**：
- `hook.py → relay`：走 `127.0.0.1` 回环，明文 HTTP（端口 8799），不出网。
- `relay ↔ StickS3`：**蓝牙 BLE**。relay 当 central 连到 stick 的 Nordic UART
  Service，两个方向都跑换行分隔的 JSON。**信任靠 BLE 配对 + 绑定（加密）**：
  首次连接配一次对即长期信任，不用 WiFi、不用证书。

## 目录

```
agent_approver/
├── relay/
│   ├── relay.py                     # Mac 上的中枢 (回环 HTTP hook 入口 + BLE 中转)
│   ├── ble.py                       # BLE central (bleak): 连 stick, 收发 JSON
│   ├── requirements.txt             # relay 蓝牙端依赖 (bleak)
│   └── com.agentapprover.relay.plist# launchd 开机自启模板
├── hooks/
│   ├── hook.py                      # Cursor hook 入口 (一脚本多模式, 只用标准库)
│   ├── hooks.template.json          # 写进 ~/.cursor/hooks.json 的模板
│   └── config.example.json          # ~/.config/agent_approver/config.json 示例
├── firmware/                        # StickS3 Arduino 工程 (M5Unified + 内置 BLE)
│   ├── firmware.ino  ble_link.*  app_prefs.*
│   ├── partitions.csv  flash.sh
└── install.sh                       # 一键装 hooks + bleak + 默认配置 (可选 launchd)
```

## 快速开始

### 1. 装 hooks + 依赖
```bash
agent_approver/install.sh
```
会把 hook 合并进 `~/.cursor/hooks.json`（保留你已有的 hooks，可重复运行）、装 `bleak`、写一份默认配置到 `~/.config/agent_approver/config.json`。**装完重启 Cursor** 生效。

### 2. 烧固件
```bash
# StickS3 长按侧面键 ~2s 进下载模式
agent_approver/firmware/flash.sh
```
烧完后 StickS3 会广播为 BLE 设备 **`AgentApprover`**，屏幕显示“等待 Mac 连接 (BLE)”。

### 3. 启动 relay（蓝牙连接 + 配对）
```bash
python3 agent_approver/relay/relay.py
```
relay 会自动扫描并连接 `AgentApprover`。**首次连接会触发 BLE 配对**，macOS 可能弹一次确认 / 提示授权“蓝牙”权限（系统设置 → 隐私与安全性 → 蓝牙）。配对成功后 StickS3 顶栏显示 `BLE ·`（绿）。

想开机自启：`agent_approver/install.sh --launchd`（注意 launchd 进程也要有蓝牙权限）。

> 连接更快更稳：先看 relay 日志里 `BLE 已连接 <地址>`，把该地址用
> `python3 relay/relay.py --ble-address <地址>` 传进去可跳过扫描。

## 用法

- **危险命令**（`rm -rf` / `sudo` / `git push --force` / `dd` / `mkfs` …）→ StickS3 亮屏显示命令，**A 批准 / B 拒绝**（审批屏会盖住总览，优先处理）。
- **普通操作** → 直接放行，只更新对应 agent 的状态。
- **总览屏**：每个 agent 一行 = 状态圆点（🟠运行 / 🔴等待审批 / 🟢空闲）+ 标签 + 当前在做什么；选中行高亮。
- **StickS3 没连上蓝牙 / relay 没起 / 超时** → 回退到 Cursor 原生审批（`ask`），不阻塞。

### StickS3 按键
| 场景 | A 单击 | A 双击 | B 短按 | A 长按 | B 长按 |
|---|---|---|---|---|---|
| 有待审批 | 翻页（看长命令） | **批准** | 拒绝 | — | 清除 BLE 绑定 |
| 总览屏 | 选下一个 agent | — | 进入选中 agent 详情 | 切屏幕亮度 | 清除 BLE 绑定 |
| 详情屏 | 翻页看总结 | — | 返回总览 | 切屏幕亮度 | 清除 BLE 绑定 |

> 审批屏：单击 A 翻页阅读完整命令/参数，**双击 A 才会批准**（防误触）。命令很长时右下角会显示 `A翻页 1/3` 这样的页码。
>
> 详情屏：agent 完成任务后，进入它的详情页会显示 **agent 返回的总结**，单击 A 逐页翻看，B 返回总览。

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

`url` 是 hook → relay 的回环地址，保持不变即可。蓝牙连接由 relay 负责，hook 不碰蓝牙。

危险命令的判定见 `hooks/hook.py` 里的 `BUILTIN_RISKY`，`risky_patterns` 会在其基础上追加。

## 蓝牙与配对

- StickS3 广播名默认 `AgentApprover`（改名见 `firmware.ino` 的 `BLE_NAME` 和 relay 的 `--ble-name`）。
- 信任：BLE 配对 + 绑定（加密，Just Works，无需输码）。绑定信息存在双方：
  - StickS3 存在 NVS —— **长按 B** 可清掉，强制重新配对；
  - Mac 存在系统蓝牙里 —— 需要时在「系统设置 → 蓝牙」里移除设备。
- 换了设备 / 配对乱了：StickS3 长按 B 清绑定 + Mac 蓝牙里移除，再重连即可。

## 支持其它 agent（claude-code / codex）

relay 的 hook 接口是 agent 无关的。接新 agent 只要让它的 hook 机制调到同样的 HTTP：
- 批准：`POST /hook/approval {agent_id,agent,tool,title,detail,cwd,timeout_ms}` → `{decision}`
- 状态：`POST /hook/status {agent_id,label,source,state,kind,text,cwd,summary}`（`state` = `busy|idle|end`，`end` 表示该 agent 结束、从总览移除；`summary` 是完成后详情页翻页看的总结）

`agent_id` 用来在总览里区分多个并发 agent（Cursor 用 `conversation_id`，Claude 用 `session_id`）。

### Cursor
`install.sh` 把 `hooks/hooks.template.json`（覆盖全部 hook 事件）合并进 `~/.cursor/hooks.json`。

### Claude Code
`install.sh` 检测到 `~/.claude`（或 PATH 里有 `claude`）时，会把 `hooks/claude_settings.template.json` 合并进 `~/.claude/settings.json`，复用同一个 `hook.py`（`hook.py claude <event>`，并设 `AGENT_APPROVER_AGENT=claude`）。

**Claude 只做状态显示，不负责审批**（审批交给 Claude 自己的权限系统）：
- 所有工具只在 StickS3 总览/详情里上报状态，不拦截、不弹审批；
- 多个 Claude 会话按 `session_id` 各算一个 agent；
- 完成时（`Stop`）用 `last_assistant_message` 当总结，详情页可翻页看。
新开一个 Claude 会话即生效。

## 卸载
```bash
agent_approver/install.sh --uninstall
```
（移除写进 `~/.cursor/hooks.json` 的条目 + launchd；保留你的配置文件。）
