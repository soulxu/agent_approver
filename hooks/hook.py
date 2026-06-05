#!/usr/bin/env python3
# =============================================================================
# agent_approver hook - Cursor (以及之后 claude-code / codex) 的 hook 入口.
#
# 一个脚本多种模式, 模式由命令行第 1 个参数决定:
#
#   hook.py shell         beforeShellExecution: 危险命令 -> 发到 StickS3 等批准;
#                         安全命令 -> 直接放行 + 上报活动. 返回 permission JSON.
#   hook.py mcp           beforeMCPExecution: 按配置决定是否要批准 (默认只上报).
#   hook.py activity edit      afterFileEdit   -> 上报 "编辑 <file>"
#   hook.py activity read      beforeReadFile  -> 上报 "读取 <file>"
#   hook.py activity prompt    beforeSubmitPrompt -> 上报 "任务: <prompt>"
#   hook.py activity stop      stop            -> 上报 "完成 / 空闲"
#
# StickS3 离线 / relay 没起 / 超时  -> 回退 (默认 "ask", 即交回 Cursor 原生审批),
# 绝不会把 agent 卡死.
#
# 配置 (可选): ~/.config/agent_approver/config.json  (见 config.example.json)
# 只用标准库, 兼容 Python 3.9.
# =============================================================================
from __future__ import annotations

import json
import os
import re
import sys
import urllib.request
from pathlib import Path

CONFIG_PATH = Path(
    os.environ.get("AGENT_APPROVER_CONFIG")
    or "~/.config/agent_approver/config.json"
).expanduser()

DEFAULTS = {
    "url": "http://127.0.0.1:8799",
    "approval_timeout_ms": 150000,
    "shell_mode": "risky",   # "risky" | "all" | "off"
    "mcp_mode": "off",       # "all" | "off"
    "fallback": "ask",       # relay/stick 不可用时: "ask" | "allow" | "deny"
    "activity": True,
    "risky_patterns": [],    # 追加到内置列表的额外正则
}

# 内置 "危险命令" 正则 (大小写不敏感). 命中任意一条 -> 需要 StickS3 批准.
BUILTIN_RISKY = [
    r"\brm\s+(-[a-z]*r[a-z]*f|-[a-z]*f[a-z]*r|-r\s+-f|-f\s+-r)\b",
    r"\brm\s+-[a-z]*r",            # 任意 rm -r...
    r"\bsudo\b",
    r"\bgit\s+push\b",
    r"\bgit\s+reset\s+--hard\b",
    r"\bgit\s+clean\s+-[a-z]*f",
    r"\bgit\s+checkout\s+--\s+\.",
    r"\bgit\s+branch\s+-D\b",
    r"--force\b|--hard\b|-f\b.*\bpush\b",
    r"\bchmod\s+-[a-z]*R",
    r"\bchown\s+-[a-z]*R",
    r"\bdd\s+if=",
    r"\bmkfs\b",
    r"\bdiskutil\s+(erase|partition|reformat)",
    r"\b(shutdown|reboot|halt)\b",
    r"\b(killall|pkill)\b",
    r"\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba)?sh",   # 管道喂 shell
    r">\s*/dev/(sd|disk|null)?",                      # 写 /dev
    r"\bnpm\s+publish\b",
    r"\bbrew\s+uninstall\b",
    r"\bshred\b|\btruncate\b",
    r":\(\)\s*\{",                                    # fork bomb
    r"\bsecurity\s+delete",
    r"\bdefaults\s+delete\b",
]


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    try:
        if CONFIG_PATH.is_file():
            user = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if isinstance(user, dict):
                cfg.update(user)
    except Exception:  # noqa: BLE001  - 配置坏了也不能挡住 agent
        pass
    return cfg


def agent_name() -> str:
    return os.environ.get("AGENT_APPROVER_AGENT", "cursor")


def read_event() -> dict:
    try:
        raw = sys.stdin.read()
        return json.loads(raw) if raw.strip() else {}
    except Exception:  # noqa: BLE001
        return {}


def event_cwd(ev: dict) -> str:
    for k in ("cwd", "workspace", "workspaceRoot"):
        v = ev.get(k)
        if isinstance(v, str) and v:
            return v
    roots = ev.get("workspace_roots") or ev.get("workspaceRoots")
    if isinstance(roots, list) and roots:
        first = roots[0]
        if isinstance(first, str):
            return first
        if isinstance(first, dict):
            return str(first.get("path") or first.get("uri") or "")
    return os.getcwd()


# ----- HTTP 小工具 (stdlib, 失败不抛) -----
def post_json(url: str, obj: dict, timeout: float) -> "dict | None":
    data = json.dumps(obj).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception:  # noqa: BLE001
        return None


def send_activity(cfg: dict, kind: str, text: str, cwd: str) -> None:
    if not cfg.get("activity", True):
        return
    post_json(
        cfg["url"].rstrip("/") + "/hook/activity",
        {"agent": agent_name(), "kind": kind, "text": text, "cwd": cwd},
        timeout=1.5,
    )


def request_approval(cfg: dict, tool: str, title: str, detail: str, cwd: str) -> str:
    timeout_ms = int(cfg.get("approval_timeout_ms", 150000))
    resp = post_json(
        cfg["url"].rstrip("/") + "/hook/approval",
        {
            "agent": agent_name(),
            "tool": tool,
            "title": title,
            "detail": detail,
            "cwd": cwd,
            "timeout_ms": timeout_ms,
        },
        timeout=timeout_ms / 1000.0 + 15.0,
    )
    if not resp:
        return "unavailable"
    return str(resp.get("decision") or "unavailable")


# ----- permission 输出 -----
def emit(permission: str, user_message: str = "", agent_message: str = "") -> None:
    out = {"permission": permission}
    if user_message:
        out["user_message"] = user_message
    if agent_message:
        out["agent_message"] = agent_message
    print(json.dumps(out))


def fallback_permission(cfg: dict, reason: str) -> None:
    fb = str(cfg.get("fallback", "ask"))
    if fb not in ("ask", "allow", "deny"):
        fb = "ask"
    emit(
        fb,
        user_message=f"Agent Approver ({reason}); 回退到 {fb}.",
        agent_message=f"StickS3 approver {reason}; fell back to {fb}.",
    )


def decision_to_permission(cfg: dict, decision: str, what: str) -> None:
    if decision == "allow":
        emit("allow")
    elif decision == "deny":
        emit(
            "deny",
            user_message=f"你在 StickS3 上拒绝了: {what}",
            agent_message="User denied this action on the StickS3 approver.",
        )
    else:  # timeout | unavailable | 其它
        fallback_permission(cfg, decision)


# =============================================================================
# 模式
# =============================================================================
def is_risky(command: str, cfg: dict) -> bool:
    pats = list(BUILTIN_RISKY) + list(cfg.get("risky_patterns") or [])
    for p in pats:
        try:
            if re.search(p, command, re.IGNORECASE):
                return True
        except re.error:
            continue
    return False


def mode_shell(cfg: dict) -> None:
    ev = read_event()
    command = str(ev.get("command") or ev.get("commandLine") or "").strip()
    cwd = event_cwd(ev)
    shell_mode = str(cfg.get("shell_mode", "risky"))

    if shell_mode == "off" or not command:
        send_activity(cfg, "shell", "$ " + command, cwd)
        emit("allow")
        return

    need = shell_mode == "all" or is_risky(command, cfg)
    if not need:
        send_activity(cfg, "shell", "$ " + command, cwd)
        emit("allow")
        return

    title = "$ " + (command if len(command) <= 120 else command[:117] + "...")
    decision = request_approval(cfg, "shell", title, command, cwd)
    decision_to_permission(cfg, decision, title)


def mode_mcp(cfg: dict) -> None:
    ev = read_event()
    tool = str(ev.get("tool_name") or ev.get("toolName") or ev.get("name") or "mcp")
    cwd = event_cwd(ev)
    raw_args = ev.get("tool_input") or ev.get("arguments") or ev.get("input") or {}
    try:
        detail = json.dumps(raw_args, ensure_ascii=False)[:500]
    except Exception:  # noqa: BLE001
        detail = str(raw_args)[:500]

    if str(cfg.get("mcp_mode", "off")) != "all":
        send_activity(cfg, "mcp", tool, cwd)
        emit("allow")
        return

    decision = request_approval(cfg, "mcp", f"MCP: {tool}", detail, cwd)
    decision_to_permission(cfg, decision, f"MCP {tool}")


def mode_activity(cfg: dict, label: str) -> None:
    ev = read_event()
    cwd = event_cwd(ev)
    if label == "edit":
        f = ev.get("file_path") or ev.get("filePath") or ev.get("path") or ""
        send_activity(cfg, "edit", "编辑 " + _short_path(str(f)), cwd)
    elif label == "read":
        f = ev.get("file_path") or ev.get("filePath") or ev.get("path") or ""
        send_activity(cfg, "read", "读取 " + _short_path(str(f)), cwd)
    elif label == "prompt":
        p = str(ev.get("prompt") or ev.get("text") or "").strip().replace("\n", " ")
        if len(p) > 160:
            p = p[:157] + "..."
        send_activity(cfg, "prompt", "任务: " + p, cwd)
    elif label == "stop":
        send_activity(cfg, "stop", "完成, 空闲中", cwd)
    else:
        send_activity(cfg, label, str(ev.get("text") or label), cwd)
    # activity 类事件不需要返回 permission, 直接结束.


def _short_path(p: str) -> str:
    if not p:
        return "(file)"
    parts = p.replace("\\", "/").split("/")
    return "/".join(parts[-2:]) if len(parts) > 2 else p


def main() -> int:
    cfg = load_config()
    args = sys.argv[1:]
    mode = args[0] if args else ""
    try:
        if mode == "shell":
            mode_shell(cfg)
        elif mode == "mcp":
            mode_mcp(cfg)
        elif mode == "activity":
            mode_activity(cfg, args[1] if len(args) > 1 else "")
        else:
            # 未知模式: 安全起见放行, 不挡 agent.
            emit("allow")
    except Exception:  # noqa: BLE001 - 任何异常都别挡住 agent
        if mode in ("shell", "mcp"):
            fallback_permission(cfg, "hook error")
    return 0


if __name__ == "__main__":
    sys.exit(main())
