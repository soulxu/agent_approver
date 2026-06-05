#!/usr/bin/env python3
# =============================================================================
# agent_approver relay - Mac 上跑的小 HTTP 中枢, 把 "agent 的审批请求 / 活动状态"
# 转发给 M5StickS3, 再把 StickS3 上按键的批准/拒绝结果回传给 agent 的 hook.
#
# 为什么要一个中枢 (而不是 hook 直接打 StickS3):
#   - hook 跑在 Mac 上, 打 127.0.0.1 又快又稳, 不用知道 StickS3 的 LAN IP;
#   - StickS3 只会 "往外连" (跟 stick_s3_eyes 的 bridge 一样), 用 long-poll
#     拉取待办, 不需要在 stick 上跑被外部访问的 server;
#   - 中枢能在 StickS3 离线时立刻回退 (hook -> Cursor 原生审批), 不会把 agent
#     卡死.
#
# 链路:
#   Cursor/Claude/Codex agent
#        │  hook (beforeShellExecution / afterFileEdit / stop / ...)
#        ▼
#   hook.py  ──HTTP 127.0.0.1:8799──▶  relay.py
#                                          │  pending 审批 + 最新活动
#                                          ▲
#                                          │  HTTP LAN long-poll
#                                     M5StickS3 (WiFi)
#                                       GET  /stick/poll?v=<ver>&wait=25
#                                       POST /stick/decide {id, decision}
#
# 只用 Python 标准库, 一行 pip 都不装. 兼容 Python 3.9 (macOS 自带).
# =============================================================================
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import uuid
from collections import OrderedDict
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, List, Optional


# StickS3 屏幕窄, detail 太长没意义, 这里先裁一刀 (UTF-8 字节). 标题更短.
MAX_DETAIL_BYTES = 600
MAX_TITLE_BYTES = 160
MAX_ACTIVITY_BYTES = 400

# 多久没收到 StickS3 的 poll 就认为它离线. long-poll 默认 25s, 给 2.5 倍余量.
STICK_OFFLINE_SEC = 60.0


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
    tool: str
    title: str
    detail: str
    cwd: str
    created: float
    timeout_ms: int
    event: threading.Event = field(default_factory=threading.Event)
    decision: Optional[str] = None  # "allow" | "deny"


class State:
    def __init__(self) -> None:
        self.cond = threading.Condition()
        self.version = 1
        self.pending: "OrderedDict[str, Approval]" = OrderedDict()
        self.activity: Optional[dict] = None
        self.recent: List[dict] = []  # 给状态网页看的活动 ring
        self.last_stick_poll = 0.0

    # ---- 内部: 改了状态就 bump version + 唤醒所有 long-poll ----
    def _bump_locked(self) -> None:
        self.version += 1
        self.cond.notify_all()

    def stick_online(self) -> bool:
        return (time.time() - self.last_stick_poll) < STICK_OFFLINE_SEC

    # ---- hook 侧: 新增一个待审批, 阻塞等结果 ----
    def submit_approval(self, ap: Approval) -> str:
        with self.cond:
            online = self.stick_online()
            if not online:
                return "unavailable"
            self.pending[ap.id] = ap
            self.activity = {
                "agent": ap.agent,
                "kind": "approval",
                "text": ap.title,
                "cwd": ap.cwd,
                "ts": ap.created,
            }
            self._bump_locked()
        log(f"approval+ id={ap.id[:8]} agent={ap.agent} tool={ap.tool} :: {ap.title}")

        deadline = ap.created + ap.timeout_ms / 1000.0
        # 等按键结果; 同时定期检查超时 / stick 掉线.
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            if ap.event.wait(timeout=min(remaining, 2.0)):
                break
            with self.cond:
                if not self.stick_online():
                    # stick 中途掉线, 别再傻等到超时.
                    self._remove_locked(ap.id)
                    log(f"approval~ id={ap.id[:8]} stick offline -> unavailable")
                    return "unavailable"

        with self.cond:
            self._remove_locked(ap.id)
            if ap.decision in ("allow", "deny"):
                log(f"approval= id={ap.id[:8]} -> {ap.decision}")
                return ap.decision
            log(f"approval= id={ap.id[:8]} -> timeout")
            return "timeout"

    def _remove_locked(self, ap_id: str) -> None:
        if ap_id in self.pending:
            del self.pending[ap_id]
            self._bump_locked()

    # ---- stick 侧: 给出决定 ----
    def decide(self, ap_id: str, decision: str) -> bool:
        with self.cond:
            ap = self.pending.get(ap_id)
            if ap is None:
                return False
            ap.decision = decision
            ap.event.set()
            self._bump_locked()
        log(f"decide  id={ap_id[:8]} <- {decision} (from stick)")
        return True

    # ---- hook 侧: 更新活动 (非阻塞) ----
    def set_activity(self, agent: str, kind: str, text: str, cwd: str) -> None:
        item = {
            "agent": agent,
            "kind": kind,
            "text": _trim_utf8(text, MAX_ACTIVITY_BYTES),
            "cwd": cwd,
            "ts": time.time(),
        }
        with self.cond:
            self.activity = item
            self.recent.append(item)
            if len(self.recent) > 50:
                self.recent = self.recent[-50:]
            self._bump_locked()

    # ---- stick 侧: 取快照 (oldest pending + latest activity) ----
    def snapshot_locked(self) -> dict:
        ap_obj = None
        if self.pending:
            first_id = next(iter(self.pending))
            ap = self.pending[first_id]
            ap_obj = {
                "id": ap.id,
                "agent": ap.agent,
                "tool": ap.tool,
                "title": ap.title,
                "detail": ap.detail,
                "cwd": ap.cwd,
                "count": len(self.pending),
                "age_ms": int((time.time() - ap.created) * 1000),
                "timeout_ms": ap.timeout_ms,
            }
        return {
            "version": self.version,
            "now": time.time(),
            "approval": ap_obj,
            "activity": self.activity,
        }

    def poll(self, known_version: int, wait_sec: float) -> dict:
        with self.cond:
            self.last_stick_poll = time.time()
            if self.version == known_version and wait_sec > 0:
                self.cond.wait(timeout=wait_sec)
            return self.snapshot_locked()


STATE = State()


# =============================================================================
# HTTP handler
# =============================================================================
class Handler(BaseHTTPRequestHandler):
    server_version = "agent_approver_relay/1.0"

    def log_message(self, fmt: str, *args) -> None:  # 静音默认日志
        pass

    # ---- 小工具 ----
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

    # ---- GET ----
    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/healthz":
            self._text(200, "ok")
            return
        if path == "/stick/poll":
            self._handle_poll()
            return
        if path == "/":
            self._handle_status_page()
            return
        self._json(404, {"err": f"unknown path: {self.path}"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/hook/approval":
            self._handle_approval()
            return
        if self.path == "/hook/activity":
            self._handle_activity()
            return
        if self.path == "/stick/decide":
            self._handle_decide()
            return
        self._json(404, {"err": f"unknown path: {self.path}"})

    # ---- /hook/approval (阻塞) ----
    def _handle_approval(self) -> None:
        req = self._read_json()
        if req is None:
            return
        title = _trim_utf8(str(req.get("title") or "").strip() or "(no title)", MAX_TITLE_BYTES)
        detail = _trim_utf8(str(req.get("detail") or "").strip(), MAX_DETAIL_BYTES)
        ap = Approval(
            id=str(req.get("id") or uuid.uuid4().hex),
            agent=str(req.get("agent") or "agent")[:32],
            tool=str(req.get("tool") or "")[:48],
            title=title,
            detail=detail,
            cwd=str(req.get("cwd") or "")[:200],
            created=time.time(),
            timeout_ms=int(req.get("timeout_ms") or 150000),
        )
        decision = STATE.submit_approval(ap)
        self._json(200, {"decision": decision, "id": ap.id})

    # ---- /hook/activity (快) ----
    def _handle_activity(self) -> None:
        req = self._read_json()
        if req is None:
            return
        STATE.set_activity(
            agent=str(req.get("agent") or "agent")[:32],
            kind=str(req.get("kind") or "")[:24],
            text=str(req.get("text") or ""),
            cwd=str(req.get("cwd") or "")[:200],
        )
        self._json(200, {"ok": True})

    # ---- /stick/decide ----
    def _handle_decide(self) -> None:
        req = self._read_json(max_len=4096)
        if req is None:
            return
        ap_id = str(req.get("id") or "")
        decision = str(req.get("decision") or "")
        if decision not in ("allow", "deny"):
            self._json(400, {"err": "decision must be allow|deny"})
            return
        ok = STATE.decide(ap_id, decision)
        self._json(200, {"ok": ok})

    # ---- /stick/poll (long-poll) ----
    def _handle_poll(self) -> None:
        qs = {}
        if "?" in self.path:
            for kv in self.path.split("?", 1)[1].split("&"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    qs[k] = v
        try:
            known_version = int(qs.get("v", "0"))
        except ValueError:
            known_version = 0
        try:
            wait_sec = max(0.0, min(30.0, float(qs.get("wait", "25"))))
        except ValueError:
            wait_sec = 25.0
        snap = STATE.poll(known_version, wait_sec)
        self._json(200, snap)

    # ---- 状态网页 ----
    def _handle_status_page(self) -> None:
        with STATE.cond:
            snap = STATE.snapshot_locked()
            recent = list(STATE.recent[-20:])
            online = STATE.stick_online()
        rows = "".join(
            f"<tr><td>{time.strftime('%H:%M:%S', time.localtime(i['ts']))}</td>"
            f"<td>{_esc(i['agent'])}</td><td>{_esc(i['kind'])}</td>"
            f"<td>{_esc(i['text'])}</td></tr>"
            for i in reversed(recent)
        )
        ap = snap.get("approval")
        ap_html = (
            f"<p><b>待审批:</b> [{_esc(ap['agent'])}] {_esc(ap['title'])} "
            f"(共 {ap['count']} 条)</p>" if ap else "<p>无待审批</p>"
        )
        html = (
            "<!doctype html><meta charset=utf-8><title>agent_approver relay</title>"
            "<style>body{font-family:-apple-system,system-ui,sans-serif;max-width:780px;"
            "margin:20px auto;padding:0 12px;color:#222}table{width:100%;border-collapse:collapse}"
            "td,th{border-bottom:1px solid #eee;padding:4px 6px;font-size:13px;text-align:left}"
            ".s{padding:6px 10px;border-radius:6px;color:#fff;display:inline-block}"
            "</style><h2>agent_approver relay</h2>"
            f"<p>StickS3: <span class=s style='background:{'#2e7d32' if online else '#c62828'}'>"
            f"{'online' if online else 'OFFLINE'}</span> &nbsp; version={snap['version']}</p>"
            f"{ap_html}<h3>最近活动</h3><table>"
            "<tr><th>time</th><th>agent</th><th>kind</th><th>text</th></tr>"
            f"{rows}</table>"
        )
        self._text(200, html, "text/html; charset=utf-8")


def _esc(s: str) -> str:
    return (
        str(s)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


# =============================================================================
# LAN IP 探测 (给用户填到 StickS3 的 relay URL)
# =============================================================================
def _ipconfig_getifaddr(iface: str) -> Optional[str]:
    try:
        r = subprocess.run(
            ["ipconfig", "getifaddr", iface],
            capture_output=True, text=True, timeout=2,
        )
        return r.stdout.strip() or None
    except Exception:  # noqa: BLE001
        return None


def find_lan_ip() -> str:
    for iface in ("en0", "en1"):
        ip = _ipconfig_getifaddr(iface)
        if ip:
            return ip
    try:
        r = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=2)
        cur = None
        skip = False
        iface_re = re.compile(r"^([a-z][a-z0-9]*): ")
        for line in r.stdout.splitlines():
            m = iface_re.match(line)
            if m:
                cur = m.group(1)
                skip = cur == "lo0" or cur.startswith(
                    ("utun", "ipsec", "ppp", "gif", "stf", "awdl", "llw", "anpi", "bridge")
                )
                continue
            if skip or cur is None:
                continue
            s = line.strip()
            if s.startswith("inet ") and not s.startswith("inet6"):
                return s.split()[1]
    except Exception:  # noqa: BLE001
        pass
    return "?"


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="agent_approver relay")
    ap.add_argument("--host", default=os.environ.get("AGENT_APPROVER_HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("AGENT_APPROVER_PORT", "8799")))
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    lan = find_lan_ip()
    log(f"listening on http://{args.host}:{args.port}")
    log(f"hooks  -> http://127.0.0.1:{args.port}")
    log(f"StickS3 relay URL -> http://{lan}:{args.port}")
    log(f"status page -> http://127.0.0.1:{args.port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
