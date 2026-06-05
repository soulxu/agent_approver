#!/usr/bin/env python3
# =============================================================================
# agent_approver relay (BLE 版) - Mac 上跑的小中枢, 把 "agent 的审批请求 / 活动
# 状态" 通过 *蓝牙* 推给 M5StickS3, 再把 StickS3 上按键的批准/拒绝结果回传给
# agent 的 hook.
#
# 链路 (两段):
#   Cursor/Claude/Codex agent
#        │  hook (beforeShellExecution / afterFileEdit / stop / ...)
#        ▼
#   hook.py ──明文 HTTP 127.0.0.1:8799──▶ relay.py (回环, 只本机可达)
#                                          │  pending 审批 + 最新活动
#                                          ▲ BLE (Nordic UART Service)
#                                          │  relay 当 central 主动连过去
#                                     M5StickS3 (BLE 外设 "AgentApprover")
#
# 信任: 首次 BLE 配对+绑定 (加密), 之后长期信任, 不用 WiFi / 不用证书.
# StickS3 离线 (没连上 BLE) -> hook 立刻回退 Cursor 原生审批, 不卡死 agent.
#
# 依赖: 蓝牙部分用 bleak (pip 装). hook 那段只用标准库. bleak 没装时 relay 仍能
# 起 (hook 会一直回退). 兼容 Python 3.9+.
# =============================================================================
from __future__ import annotations

import argparse
import json
import os
import queue
import sys
import threading
import time
import uuid
from collections import OrderedDict
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

try:
    import ble as ble_mod
except Exception:  # noqa: BLE001
    ble_mod = None


# StickS3 屏幕窄, 也受 BLE 单包大小限制, 裁一刀.
MAX_DETAIL_BYTES = 800
MAX_TITLE_BYTES = 120
MAX_SUMMARY_BYTES = 1200
MAX_ACTIVITY_BYTES = 300


def _ts() -> str:
    return time.strftime("%H:%M:%S")


def log(msg: str) -> None:
    print(f"[{_ts()}] {msg}", flush=True)


def _trim_utf8(s: str, max_bytes: int) -> str:
    b = s.encode("utf-8")
    if len(b) <= max_bytes:
        return s
    b = b[:max_bytes]
    for back in range(4):
        try:
            return b[: len(b) - back].decode("utf-8")
        except UnicodeDecodeError:
            continue
    return ""


# =============================================================================
# 共享状态
# =============================================================================
@dataclass
class Approval:
    id: str
    agent: str
    agent_id: str
    tool: str
    title: str
    detail: str
    cwd: str
    created: float
    timeout_ms: int
    event: threading.Event = field(default_factory=threading.Event)
    decision: Optional[str] = None  # "allow" | "deny"


MAX_AGENTS = 16
MAX_LABEL_BYTES = 48


class State:
    def __init__(self) -> None:
        self.cond = threading.Condition()
        self.pending: "OrderedDict[str, Approval]" = OrderedDict()
        # 每个 agent (按 conversation_id) 的当前状态: id -> info dict
        self.agents: "OrderedDict[str, dict]" = OrderedDict()
        self.ble_connected = False
        # 待发往 stick 的 BLE 消息 (BLE worker 线程消费)
        self.outbox: "queue.Queue[dict]" = queue.Queue()

    # ---- BLE 消息构造 ----
    @staticmethod
    def _agent_msg(a: dict) -> dict:
        return {
            "t": "agent", "id": a["id"], "label": a["label"],
            "state": a["state"], "text": a["text"],
        }

    @staticmethod
    def _approval_msg(ap: Approval) -> dict:
        return {
            "t": "approval",
            "id": ap.id, "agent": ap.agent, "tool": ap.tool,
            "title": ap.title, "detail": ap.detail, "cwd": ap.cwd,
            "timeout_ms": ap.timeout_ms,
        }

    # ---- BLE worker 回调 ----
    def on_ble_up(self) -> None:
        with self.cond:
            self.ble_connected = True
            # 清空积压, 重置 stick, 再把当前所有 agent + 待审批同步过去
            self._drain_outbox_locked()
            self._enqueue({"t": "reset"})
            for a in self.agents.values():
                self._enqueue(self._agent_msg(a))
                if a.get("summary"):
                    self._enqueue({"t": "agent_summary", "id": a["id"], "summary": a["summary"]})
            for ap in self.pending.values():
                self._enqueue(self._approval_msg(ap))
            self.cond.notify_all()
        log("BLE stick connected")

    def on_ble_down(self) -> None:
        with self.cond:
            if not self.ble_connected:
                return
            self.ble_connected = False
            self._drain_outbox_locked()
            self.cond.notify_all()
        log("BLE stick disconnected")

    def _drain_outbox_locked(self) -> None:
        try:
            while True:
                self.outbox.get_nowait()
        except queue.Empty:
            pass

    def _enqueue(self, msg: dict) -> None:
        if self.ble_connected:
            self.outbox.put(msg)

    def stick_online(self) -> bool:
        return self.ble_connected

    def _upsert_agent_locked(self, agent_id: str, **fields) -> dict:
        a = self.agents.pop(agent_id, None) or {
            "id": agent_id, "label": "agent", "source": "",
            "state": "busy", "kind": "", "text": "", "cwd": "", "summary": "",
        }
        a.update({k: v for k, v in fields.items() if v is not None})
        a["ts"] = time.time()
        self.agents[agent_id] = a  # 移到末尾 = 最近活跃
        while len(self.agents) > MAX_AGENTS:
            self.agents.popitem(last=False)
        return a

    # ---- hook 侧: 新增一个待审批, 阻塞等结果 ----
    def submit_approval(self, ap: Approval) -> str:
        with self.cond:
            if not self.ble_connected:
                return "unavailable"
            self.pending[ap.id] = ap
            a = self._upsert_agent_locked(
                ap.agent_id, label=(ap.agent or None),
                state="wait", kind="approval", text=ap.title, cwd=ap.cwd,
            )
            self._enqueue(self._agent_msg(a))
            self._enqueue(self._approval_msg(ap))
        log(f"approval+ id={ap.id[:8]} agent={ap.agent} tool={ap.tool} :: {ap.title}")

        deadline = ap.created + ap.timeout_ms / 1000.0
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            if ap.event.wait(timeout=min(remaining, 2.0)):
                break
            with self.cond:
                if not self.ble_connected:
                    self._remove_locked(ap.id)
                    log(f"approval~ id={ap.id[:8]} ble offline -> unavailable")
                    return "unavailable"

        with self.cond:
            self._remove_locked(ap.id)
            decision = ap.decision
            a = self.agents.get(ap.agent_id)
            if a is not None:
                a["state"] = "busy"
                a["kind"] = "approval_done"
                a["text"] = ("已批准: " if decision == "allow"
                             else "已拒绝: " if decision == "deny"
                             else "审批超时: ") + ap.title
                a["ts"] = time.time()
                self._enqueue(self._agent_msg(a))
            self._enqueue({"t": "cancel", "id": ap.id})
        if decision in ("allow", "deny"):
            log(f"approval= id={ap.id[:8]} -> {decision}")
            return decision
        log(f"approval= id={ap.id[:8]} -> timeout")
        return "timeout"

    def _remove_locked(self, ap_id: str) -> None:
        if ap_id in self.pending:
            del self.pending[ap_id]

    # ---- BLE 侧: stick 给出决定 ----
    def decide(self, ap_id: str, decision: str) -> bool:
        if decision not in ("allow", "deny"):
            return False
        with self.cond:
            ap = self.pending.get(ap_id)
            if ap is None:
                return False
            ap.decision = decision
            ap.event.set()
            self.cond.notify_all()
        log(f"decide  id={ap_id[:8]} <- {decision} (from stick)")
        return True

    # ---- hook 侧: 更新某个 agent 的状态 (非阻塞) ----
    def set_status(self, agent_id: str, label: str, source: str,
                   state: str, kind: str, text: str, cwd: str,
                   summary: str = "") -> None:
        with self.cond:
            if state == "end":
                if agent_id in self.agents:
                    del self.agents[agent_id]
                    self._enqueue({"t": "agent_del", "id": agent_id})
                return
            sm = _trim_utf8(summary, MAX_SUMMARY_BYTES) if summary else None
            a = self._upsert_agent_locked(
                agent_id,
                label=(_trim_utf8(label, MAX_LABEL_BYTES) if label else None),
                source=(source or None),
                state=(state or "busy"),
                kind=kind,
                text=_trim_utf8(text, MAX_ACTIVITY_BYTES),
                cwd=cwd,
                summary=sm,
            )
            self._enqueue(self._agent_msg(a))
            # 总结单独发 (较长, 只在变化时发, 不拖慢普通状态更新)
            if sm:
                self._enqueue({"t": "agent_summary", "id": agent_id, "summary": sm})

    def status_snapshot(self) -> dict:
        with self.cond:
            agents = sorted(self.agents.values(), key=lambda x: x["ts"], reverse=True)
            return {
                "ble": self.ble_connected,
                "pending": len(self.pending),
                "agents": [dict(a) for a in agents],
            }


STATE = State()


# =============================================================================
# HTTP handler (只剩 hook 入口 + 状态页, 都跑在回环上)
# =============================================================================
class Handler(BaseHTTPRequestHandler):
    server_version = "agent_approver_relay/2.0"

    def log_message(self, fmt: str, *args) -> None:
        pass

    def _read_json(self, max_len: int = 256 * 1024) -> Optional[dict]:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0 or length > max_len:
            self._json(400, {"err": f"bad body length {length}"})
            return None
        try:
            raw = self.rfile.read(length)
            return json.loads(raw.decode("utf-8"))
        except Exception as e:  # noqa: BLE001
            self._json(400, {"err": f"bad json: {e}"})
            return None

    def _json(self, status: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _text(self, status: int, body: str, ctype: str = "text/plain; charset=utf-8") -> None:
        data = body.encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/healthz":
            self._text(200, "ok")
            return
        if path == "/":
            self._handle_status_page()
            return
        self._json(404, {"err": f"unknown path: {self.path}"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/hook/approval":
            self._handle_approval()
            return
        if self.path == "/hook/status":
            self._handle_status()
            return
        if self.path == "/hook/activity":  # 向后兼容旧 hook
            self._handle_activity()
            return
        self._json(404, {"err": f"unknown path: {self.path}"})

    def _handle_approval(self) -> None:
        req = self._read_json()
        if req is None:
            return
        title = _trim_utf8(str(req.get("title") or "").strip() or "(no title)", MAX_TITLE_BYTES)
        detail = _trim_utf8(str(req.get("detail") or "").strip(), MAX_DETAIL_BYTES)
        agent = str(req.get("agent") or "agent")[:48]
        ap = Approval(
            id=str(req.get("id") or uuid.uuid4().hex),
            agent=agent,
            agent_id=str(req.get("agent_id") or ("legacy:" + agent))[:120],
            tool=str(req.get("tool") or "")[:48],
            title=title,
            detail=detail,
            cwd=str(req.get("cwd") or "")[:200],
            created=time.time(),
            timeout_ms=int(req.get("timeout_ms") or 150000),
        )
        decision = STATE.submit_approval(ap)
        self._json(200, {"decision": decision, "id": ap.id})

    def _handle_status(self) -> None:
        req = self._read_json()
        if req is None:
            return
        label = str(req.get("label") or "agent")[:48]
        STATE.set_status(
            agent_id=str(req.get("agent_id") or ("legacy:" + label))[:120],
            label=label,
            source=str(req.get("source") or "")[:24],
            state=str(req.get("state") or "busy")[:12],
            kind=str(req.get("kind") or "")[:24],
            text=str(req.get("text") or ""),
            cwd=str(req.get("cwd") or "")[:200],
            summary=str(req.get("summary") or ""),
        )
        self._json(200, {"ok": True})

    def _handle_activity(self) -> None:
        req = self._read_json()
        if req is None:
            return
        agent = str(req.get("agent") or "agent")[:48]
        STATE.set_status(
            agent_id="legacy:" + agent,
            label=agent,
            source="",
            state="busy",
            kind=str(req.get("kind") or "")[:24],
            text=str(req.get("text") or ""),
            cwd=str(req.get("cwd") or "")[:200],
        )
        self._json(200, {"ok": True})

    def _handle_status_page(self) -> None:
        snap = STATE.status_snapshot()
        online = snap["ble"]
        rows = "".join(
            f"<tr><td>{time.strftime('%H:%M:%S', time.localtime(a['ts']))}</td>"
            f"<td>{_esc(a['label'])}</td><td>{_esc(a['state'])}</td>"
            f"<td>{_esc(a['kind'])}</td><td>{_esc(a['text'])}</td></tr>"
            for a in snap["agents"]
        )
        html = (
            "<!doctype html><meta charset=utf-8><title>agent_approver relay</title>"
            "<meta http-equiv=refresh content=3>"
            "<style>body{font-family:-apple-system,system-ui,sans-serif;max-width:820px;"
            "margin:20px auto;padding:0 12px;color:#222}table{width:100%;border-collapse:collapse}"
            "td,th{border-bottom:1px solid #eee;padding:4px 6px;font-size:13px;text-align:left}"
            ".s{padding:6px 10px;border-radius:6px;color:#fff;display:inline-block}"
            "</style><h2>agent_approver relay (BLE)</h2>"
            f"<p>StickS3: <span class=s style='background:{'#2e7d32' if online else '#c62828'}'>"
            f"{'connected' if online else 'OFFLINE'}</span> &nbsp; "
            f"agents={len(snap['agents'])} &nbsp; pending={snap['pending']}</p>"
            f"<h3>Agents</h3><table>"
            "<tr><th>updated</th><th>agent</th><th>state</th><th>kind</th><th>doing</th></tr>"
            f"{rows}</table>"
        )
        self._text(200, html, "text/html; charset=utf-8")


def _esc(s: str) -> str:
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# =============================================================================
# 入口
# =============================================================================
def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="agent_approver relay (BLE)")
    ap.add_argument("--host", default=os.environ.get("AGENT_APPROVER_HOST", "127.0.0.1"),
                    help="hook 明文监听地址 (默认 127.0.0.1 回环)")
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("AGENT_APPROVER_PORT", "8799")),
                    help="hook 明文端口 (默认 8799)")
    ap.add_argument("--ble-name", default=os.environ.get("AGENT_APPROVER_BLE_NAME", "AgentApprover"),
                    help="StickS3 的 BLE 广播名 (默认 AgentApprover)")
    ap.add_argument("--ble-address", default=os.environ.get("AGENT_APPROVER_BLE_ADDR", ""),
                    help="直接指定 stick 的 BLE 地址 (跳过扫描, 更快更稳)")
    ap.add_argument("--no-ble", action="store_true", help="不启动 BLE (调试用)")
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    hook_server = ThreadingHTTPServer((args.host, args.port), Handler)
    hook_server.daemon_threads = True

    if args.no_ble:
        log("BLE 已禁用 (--no-ble), stick 会一直离线")
    elif ble_mod is None:
        log("!! 没装 bleak, BLE 不可用 -> hook 会一直回退. 装: pip3 install bleak")
    else:
        worker = ble_mod.BleWorker(
            STATE, name=args.ble_name,
            address=(args.ble_address or None), log=log,
        )
        worker.start()
        log(f"BLE worker started (找 '{args.ble_name}')")

    log(f"hooks   -> http://127.0.0.1:{args.port}   (给 hook.py)")
    log(f"status page -> http://127.0.0.1:{args.port}/")
    try:
        hook_server.serve_forever()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        hook_server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
