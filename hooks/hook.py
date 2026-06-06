#!/usr/bin/env python3
# =============================================================================
# agent_approver hook - Cursor (以及之后 claude-code / codex) 的 hook 入口.
#
# 一个脚本多种模式, 模式由命令行第 1 个参数决定:
#
#   hook.py shell           beforeShellExecution: 危险命令 -> 发到 StickS3 等批准;
#                           安全命令 -> 直接放行 + 上报状态. 返回 permission JSON.
#   hook.py mcp             beforeMCPExecution: 按配置决定是否要批准 (默认只上报).
#   hook.py status <label>  其它所有 hook: 把 agent 当前在干什么上报给 relay.
#                           <label> 见 STATUS_SPEC, 例如 prompt/edit/read/stop/...
#
# 每个事件都带 conversation_id, 用它区分 "同时在跑的多个 agent"; 用 workspace
# 根目录名当 agent 的显示标签. relay 按 agent 聚合, StickS3 上能总览多个 agent,
# 按键钻进某一个看详情.
#
# StickS3 离线 / relay 没起 / 超时  -> 回退 (默认 "ask", 交回 Cursor 原生审批),
# 绝不会把 agent 卡死.
#
# 配置 (可选): ~/.config/agent_approver/config.json  (见 config.example.json)
# 只用标准库, 兼容 Python 3.9.
# =============================================================================
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import tempfile
import time
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
    "activity": True,        # 是否上报状态
    "risky_patterns": [],    # 追加到内置列表的额外正则
}

# 内置 "危险命令" 正则 (大小写不敏感). 命中任意一条 -> 需要 StickS3 批准.
BUILTIN_RISKY = [
    r"\brm\s+(-[a-z]*r[a-z]*f|-[a-z]*f[a-z]*r|-r\s+-f|-f\s+-r)\b",
    r"\brm\s+-[a-z]*r",
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
    r"\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba)?sh",
    r">\s*/dev/(sd|disk|null)?",
    r"\bnpm\s+publish\b",
    r"\bbrew\s+uninstall\b",
    r"\bshred\b|\btruncate\b",
    r":\(\)\s*\{",
    r"\bsecurity\s+delete",
    r"\bdefaults\s+delete\b",
]

# 各 status 事件 -> (state, response)
#   state    : 上报给屏幕的状态分类 busy/idle/end
#   response : 该 hook 要不要回 permission, 以免挡住 agent
#              "allow"=回 {permission:allow}; "continue"=回 {continue:true}; "none"=不回
STATUS_SPEC = {
    "session_start":  ("idle", "none"),
    "session_end":    ("end",  "none"),
    "prompt":         ("busy", "continue"),
    "shell_done":     ("busy", "none"),
    "mcp_done":       ("busy", "none"),
    "read":           ("busy", "allow"),
    "edit":           ("busy", "none"),
    "response":       ("busy", "none"),
    "thought":        ("busy", "none"),
    "compact":        ("busy", "none"),
    "subagent_start": ("busy", "allow"),
    "subagent_stop":  ("busy", "none"),
    "tool":           ("busy", "allow"),
    "tool_done":      ("busy", "none"),
    "tool_fail":      ("busy", "none"),
    "stop":           ("idle", "none"),
}


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    try:
        if CONFIG_PATH.is_file():
            user = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if isinstance(user, dict):
                cfg.update(user)
    except Exception:  # noqa: BLE001
        pass
    return cfg


def source_name() -> str:
    return os.environ.get("AGENT_APPROVER_AGENT", "cursor")


# ----- 标题暂存 -----
# beforeShellExecution 拿不到 agent 对命令的自然语言描述 (agent_message), 但
# preToolUse 能拿到. 所以在 preToolUse 里把 "命令 -> 描述" 暂存到临时文件,
# beforeShellExecution 再取出来当审批标题. 取不到就退回用命令头几个 token.
_STASH = os.path.join(tempfile.gettempdir(), "agent_approver_titles.json")


def _stash_key(agent: str, command: str) -> str:
    return hashlib.sha1((agent + "\n" + command).encode("utf-8")).hexdigest()[:16]


def _stash_load() -> dict:
    try:
        with open(_STASH, encoding="utf-8") as f:
            d = json.load(f)
            return d if isinstance(d, dict) else {}
    except Exception:  # noqa: BLE001
        return {}


def _stash_save(d: dict) -> None:
    try:
        tmp = _STASH + f".{os.getpid()}.tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f)
        os.replace(tmp, _STASH)
    except Exception:  # noqa: BLE001
        pass


def stash_title(agent: str, command: str, msg: str) -> None:
    if not command or not msg:
        return
    d = _stash_load()
    now = time.time()
    d = {k: v for k, v in d.items() if now - v.get("ts", 0) < 180}  # 删过期的
    d[_stash_key(agent, command)] = {"msg": msg[:120], "ts": now}
    if len(d) > 64:
        d = dict(sorted(d.items(), key=lambda kv: kv[1].get("ts", 0))[-64:])
    _stash_save(d)


def pop_title(agent: str, command: str) -> str:
    if not command:
        return ""
    v = _stash_load().get(_stash_key(agent, command))
    return str(v.get("msg") or "") if v else ""


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


def agent_id(ev: dict) -> str:
    for k in ("conversation_id", "session_id", "parent_conversation_id", "generation_id"):
        v = ev.get(k)
        if isinstance(v, str) and v:
            return v
    cwd = event_cwd(ev)
    return "ws:" + cwd if cwd else "default"


def agent_label(ev: dict) -> str:
    cwd = event_cwd(ev)
    name = ""
    if cwd:
        name = os.path.basename(cwd.rstrip("/")) or cwd
    if not name:
        name = source_name()
    return name[:24]


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


def send_status(cfg: dict, ev: dict, state: str, kind: str, text: str,
                summary: str = "") -> None:
    if not cfg.get("activity", True):
        return
    payload = {
        "agent_id": agent_id(ev),
        "label": agent_label(ev),
        "source": source_name(),
        "state": state,
        "kind": kind,
        "text": text,
        "cwd": event_cwd(ev),
    }
    if summary:
        payload["summary"] = summary  # agent 返回的总结 (完成时在详情页翻页看)
    post_json(cfg["url"].rstrip("/") + "/hook/status", payload, timeout=1.5)


def request_approval(cfg: dict, ev: dict, tool: str, title: str, detail: str) -> str:
    timeout_ms = int(cfg.get("approval_timeout_ms", 150000))
    resp = post_json(
        cfg["url"].rstrip("/") + "/hook/approval",
        {
            "agent_id": agent_id(ev),
            "agent": agent_label(ev),
            "source": source_name(),
            "tool": tool,
            "title": title,
            "detail": detail,
            "cwd": event_cwd(ev),
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
    else:
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


def shell_title(command: str) -> str:
    """从命令里抽一个简短标题 (程序名 + 子命令), 完整命令放 detail 里."""
    toks = command.split()
    if not toks:
        return "命令"
    # 跳过前置环境变量赋值 (FOO=bar cmd ...)
    idx = 0
    while idx < len(toks) and "=" in toks[idx] and not toks[idx].startswith("-"):
        idx += 1
    head = toks[idx:idx + 2] if idx < len(toks) else toks[:2]
    title = " ".join(head) if head else "命令"
    return title[:48]


def mode_shell(cfg: dict) -> None:
    ev = read_event()
    command = str(ev.get("command") or ev.get("commandLine") or "").strip()
    shell_mode = str(cfg.get("shell_mode", "risky"))

    if shell_mode == "off" or not command:
        send_status(cfg, ev, "busy", "shell", "$ " + command)
        emit("allow")
        return

    need = shell_mode == "all" or is_risky(command, cfg)
    if not need:
        send_status(cfg, ev, "busy", "shell", "$ " + command)
        emit("allow")
        return

    # 标题优先用 agent 自己的描述 (preToolUse 暂存的 agent_message), 取不到再退回命令头
    title = pop_title(agent_id(ev), command) or shell_title(command)
    decision = request_approval(cfg, ev, "shell", title, command)  # 完整命令进 detail
    decision_to_permission(cfg, decision, "$ " + command)


def mode_mcp(cfg: dict) -> None:
    ev = read_event()
    tool = str(ev.get("tool_name") or ev.get("toolName") or ev.get("name") or "mcp")
    raw_args = ev.get("tool_input") or ev.get("arguments") or ev.get("input") or {}
    try:
        detail = json.dumps(raw_args, ensure_ascii=False)[:500]
    except Exception:  # noqa: BLE001
        detail = str(raw_args)[:500]

    if str(cfg.get("mcp_mode", "off")) != "all":
        send_status(cfg, ev, "busy", "mcp", "MCP " + tool)
        emit("allow")
        return

    decision = request_approval(cfg, ev, "mcp", f"MCP: {tool}", detail)
    decision_to_permission(cfg, decision, f"MCP {tool}")


def _short_path(p: str) -> str:
    if not p:
        return "(file)"
    parts = p.replace("\\", "/").split("/")
    return "/".join(parts[-2:]) if len(parts) > 2 else p


def _clip(s: str, n: int) -> str:
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "…"


def status_text(label: str, ev: dict) -> str:
    if label == "prompt":
        return "任务: " + _clip(ev.get("prompt") or ev.get("text") or "", 140)
    if label == "thought":
        return "思考: " + _clip(ev.get("text") or "", 120)
    if label == "response":
        return "回复: " + _clip(ev.get("text") or "", 120)
    if label == "edit":
        f = ev.get("file_path") or ev.get("filePath") or ev.get("path") or ""
        return "编辑 " + _short_path(str(f))
    if label == "read":
        f = ev.get("file_path") or ev.get("filePath") or ev.get("path") or ""
        return "读取 " + _short_path(str(f))
    if label == "shell_done":
        return "完成命令 " + _clip(ev.get("command") or "", 80)
    if label == "mcp_done":
        return "完成 MCP " + str(ev.get("tool_name") or "")
    if label == "tool":
        return "调用 " + str(ev.get("tool_name") or "tool")
    if label == "tool_done":
        return "完成 " + str(ev.get("tool_name") or "tool")
    if label == "tool_fail":
        return "失败 " + str(ev.get("tool_name") or "tool") + ": " + _clip(ev.get("error_message") or "", 60)
    if label == "compact":
        return "压缩上下文 " + str(ev.get("context_usage_percent") or "") + "%"
    if label == "subagent_start":
        return "子任务: " + _clip(ev.get("task") or "", 100)
    if label == "subagent_stop":
        return "子任务完成 (" + str(ev.get("status") or "") + ")"
    if label == "session_start":
        return "会话开始 (" + str(ev.get("composer_mode") or "agent") + ")"
    if label == "session_end":
        return "会话结束"
    if label == "stop":
        return "完成, 空闲中"
    return label


def mode_status(cfg: dict, label: str) -> None:
    ev = read_event()
    spec = STATUS_SPEC.get(label, ("busy", "none"))
    state, response = spec

    # preToolUse: 暂存 agent 对命令的描述, 供随后的 beforeShellExecution 当标题
    if label == "tool":
        ti = ev.get("tool_input")
        cmd = ti.get("command") if isinstance(ti, dict) else None
        am = ev.get("agent_message") or ""
        if cmd and am:
            stash_title(agent_id(ev), str(cmd), str(am))
    # agent 的最终回复 -> 当作 "总结" 一起发, 详情页可翻页查看
    summary = ""
    if label == "response":
        summary = _clip(ev.get("text") or "", 1500)
    elif label == "subagent_stop":
        summary = _clip(ev.get("summary") or "", 1500)
    send_status(cfg, ev, state, label, status_text(label, ev), summary=summary)
    if response == "allow":
        emit("allow")
    elif response == "continue":
        print(json.dumps({"continue": True}))
    # "none": 不输出, fire-and-forget


# =============================================================================
# Claude Code / Codex 支持 (两者 hook 协议几乎一致, 共用这套 handler)
#
# 它们的 hook 协议跟 Cursor 不同, 但彼此相同: 都是 stdin 收一个 JSON,
# 共享字段 session_id / cwd / hook_event_name, 工具事件带 tool_name / tool_input.
# relay 是 agent 无关的, 这里只做一层翻译, 复用同一个 relay:
#   - agent 区分: 用 session_id (agent_id() 已会优先读它)
#   - source: 由各自模板里的 AGENT_APPROVER_AGENT=claude|codex 决定
#   - 总结: Stop 自带 last_assistant_message = 最终回复
#
# 审批走 PermissionRequest 事件 (Claude/Codex 都有): 它正好在 agent 要弹批准框
# 时触发, 我们转到 StickS3, 返回 {"decision":{"behavior":"allow|deny"}} 就直接
# 替用户决定, 不会再在 agent 里弹第二次. PreToolUse 只上报状态、不审批 (避免双弹).
# 取不到 approver 时按 fallback (默认 ask=不输出, 交回 agent 自己的批准弹窗).
# Codex 的文件编辑工具是 apply_patch (Claude 是 Edit/Write 等).
# =============================================================================
def claude_kind(tool: str) -> str:
    if tool == "Bash":
        return "shell"
    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit", "Update", "apply_patch"):
        return "edit"
    if tool == "Read":
        return "read"
    if tool.startswith("mcp__"):
        return "mcp"
    return "tool"


def claude_tool_text(tool: str, ti: dict) -> str:
    ti = ti if isinstance(ti, dict) else {}
    if tool == "Bash":
        return "$ " + _clip(ti.get("command") or "", 120)
    if tool == "apply_patch":  # Codex 的文件编辑工具
        return "编辑 apply_patch"
    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit", "Update"):
        return "编辑 " + _short_path(str(ti.get("file_path") or ti.get("notebook_path") or ""))
    if tool == "Read":
        return "读取 " + _short_path(str(ti.get("file_path") or ""))
    if tool in ("Glob", "Grep"):
        return "搜索 " + _clip(ti.get("pattern") or "", 60)
    if tool == "WebFetch":
        return "抓取 " + _clip(ti.get("url") or "", 80)
    if tool == "WebSearch":
        return "搜索 " + _clip(ti.get("query") or "", 60)
    if tool == "Task":
        return "子任务 " + _clip(ti.get("description") or "", 60)
    if tool.startswith("mcp__"):
        return "MCP " + tool
    return "调用 " + (tool or "tool")


def claude_pretool(cfg: dict) -> None:
    # PreToolUse 只上报状态, 不负责审批 (审批走 PermissionRequest, 见下).
    # 不输出任何东西 = 完全不干预 agent 执行.
    ev = read_event()
    tool = str(ev.get("tool_name") or "")
    ti = ev.get("tool_input") or {}
    send_status(cfg, ev, "busy", claude_kind(tool), claude_tool_text(tool, ti))


def emit_permission(behavior: str, message: str = "") -> None:
    # Claude / Codex 共用的 PermissionRequest 输出格式
    decision = {"behavior": behavior}
    if behavior == "deny" and message:
        decision["message"] = message
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PermissionRequest", "decision": decision}}))


def claude_permission(cfg: dict) -> None:
    # PermissionRequest: agent 真要弹批准框时才触发 -> 转到 StickS3.
    # 返回 allow/deny 就直接替用户决定, 不会再在 agent 里弹一次.
    # 取不到 approver 时不输出 (或按 fallback), 交回 agent 自己的批准弹窗.
    ev = read_event()
    tool = str(ev.get("tool_name") or "")
    ti = ev.get("tool_input") if isinstance(ev.get("tool_input"), dict) else {}
    desc = str(ti.get("description") or "").strip()

    if tool == "Bash":
        detail = str(ti.get("command") or "")
        title = desc or shell_title(detail)
        kind = "shell"
    elif tool == "apply_patch":
        detail = str(ti.get("command") or "")
        title = desc or "apply_patch"
        kind = "edit"
    else:
        try:
            detail = json.dumps(ti, ensure_ascii=False)[:800]
        except Exception:  # noqa: BLE001
            detail = str(ti)[:800]
        title = desc or claude_tool_text(tool, ti)
        kind = claude_kind(tool)

    send_status(cfg, ev, "wait", kind, "等待批准: " + title)
    decision = request_approval(cfg, ev, kind, title, detail)

    if decision == "allow":
        emit_permission("allow")
    elif decision == "deny":
        emit_permission("deny", "在 StickS3 上拒绝")
    else:
        fb = str(cfg.get("fallback", "ask"))
        if fb == "allow":
            emit_permission("allow")
        elif fb == "deny":
            emit_permission("deny", "approver 不可用")
        # "ask": 不输出 -> 交回 agent 自己的批准弹窗


def claude_status(cfg: dict, label: str) -> None:
    ev = read_event()
    if label == "prompt":
        send_status(cfg, ev, "busy", "prompt",
                    "任务: " + _clip(ev.get("prompt") or ev.get("user_prompt") or "", 140))
    elif label == "posttool":
        tool = str(ev.get("tool_name") or "")
        send_status(cfg, ev, "busy", claude_kind(tool), "完成 " + (tool or "tool"))
    elif label == "stop":
        summary = _clip(ev.get("last_assistant_message") or "", 1500)
        send_status(cfg, ev, "idle", "stop", "完成, 空闲中", summary=summary)
    elif label == "posttool_fail":
        tool = str(ev.get("tool_name") or "")
        send_status(cfg, ev, "busy", claude_kind(tool), "失败 " + (tool or "tool"))
    elif label == "subagent_start":
        send_status(cfg, ev, "busy", "subagent_start",
                    "子任务开始 (" + str(ev.get("agent_type") or "") + ")")
    elif label == "subagent_stop":
        send_status(cfg, ev, "busy", "subagent_stop", "子任务完成")
    elif label == "precompact":
        send_status(cfg, ev, "busy", "compact", "压缩上下文 (" + str(ev.get("trigger") or "") + ")")
    elif label == "postcompact":
        send_status(cfg, ev, "busy", "compact", "压缩完成")
    elif label == "setup":
        send_status(cfg, ev, "idle", "setup", "初始化 (" + str(ev.get("trigger") or "") + ")")
    elif label == "session_start":
        send_status(cfg, ev, "idle", "session_start", "会话开始 (" + str(ev.get("source") or "") + ")")
    elif label == "session_end":
        send_status(cfg, ev, "end", "session_end", "会话结束")
    elif label == "stopfail":
        send_status(cfg, ev, "idle", "stopfail", "回合出错 (" + str(ev.get("matcher") or "") + ")")
    elif label == "notification":
        send_status(cfg, ev, "busy", "notification", _clip(ev.get("message") or "", 120))


def main() -> int:
    cfg = load_config()
    args = sys.argv[1:]
    mode = args[0] if args else ""
    try:
        if mode == "shell":
            mode_shell(cfg)
        elif mode == "mcp":
            mode_mcp(cfg)
        elif mode == "status":
            mode_status(cfg, args[1] if len(args) > 1 else "")
        elif mode == "activity":  # 向后兼容旧模板
            mode_status(cfg, args[1] if len(args) > 1 else "")
        elif mode == "claude":
            sub = args[1] if len(args) > 1 else ""
            if sub == "pretool":
                claude_pretool(cfg)
            elif sub == "permission":
                claude_permission(cfg)
            else:
                claude_status(cfg, sub)
        else:
            emit("allow")
    except Exception:  # noqa: BLE001 - 任何异常都别挡住 agent
        if mode in ("shell", "mcp"):
            fallback_permission(cfg, "hook error")
        # claude / status: 出错就不输出, 交给 agent 自己处理, 别卡住
    return 0


if __name__ == "__main__":
    sys.exit(main())
