#!/usr/bin/env bash
# =============================================================================
# agent_approver 安装脚本.
#
#   ./install.sh              安装 hooks 到 ~/.cursor/hooks.json + 写默认配置
#   ./install.sh --launchd    额外把 relay 装成 launchd (开机自启)
#   ./install.sh --uninstall  移除本项目装进 ~/.cursor/hooks.json 的 hook + launchd
#
# 幂等: 重复跑只会刷新自己的条目, 不动你已有的其它 hooks.
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOOK="$HERE/hooks/hook.py"
RELAY="$HERE/relay/relay.py"
PYTHON="$(command -v python3)"
CURSOR_HOOKS="$HOME/.cursor/hooks.json"
CLAUDE_SETTINGS="$HOME/.claude/settings.json"
CODEX_HOOKS="$HOME/.codex/hooks.json"
CFG_DIR="$HOME/.config/agent_approver"
CFG="$CFG_DIR/config.json"
LAUNCH_PLIST="$HOME/Library/LaunchAgents/com.agentapprover.relay.plist"
LOG="$CFG_DIR/relay.log"

uninstall() {
  echo "[uninstall] 从 $CURSOR_HOOKS 移除 agent_approver hook"
  if [[ -f "$CURSOR_HOOKS" ]]; then
    HOOK="$HOOK" "$PYTHON" - "$CURSOR_HOOKS" <<'PY'
import json, os, sys
path = sys.argv[1]
hook = os.environ["HOOK"]
data = json.load(open(path))
for ev, arr in list(data.get("hooks", {}).items()):
    arr = [h for h in arr if hook not in (h.get("command") or "")]
    if arr:
        data["hooks"][ev] = arr
    else:
        del data["hooks"][ev]
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print("  done")
PY
  fi
  if [[ -f "$CLAUDE_SETTINGS" ]]; then
    echo "[uninstall] 从 $CLAUDE_SETTINGS 移除 agent_approver hook"
    HOOK="$HOOK" "$PYTHON" - "$CLAUDE_SETTINGS" <<'PY'
import json, os, sys
path = sys.argv[1]
hook = os.environ["HOOK"]
try:
    data = json.load(open(path))
except Exception:
    sys.exit(0)
hooks = data.get("hooks", {})
for ev, groups in list(hooks.items()):
    kept = []
    for g in groups:
        hs = [h for h in g.get("hooks", []) if hook not in (h.get("command") or "")]
        if hs:
            g = dict(g); g["hooks"] = hs; kept.append(g)
    if kept:
        hooks[ev] = kept
    else:
        del hooks[ev]
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print("  done")
PY
  fi
  if [[ -f "$CODEX_HOOKS" ]]; then
    echo "[uninstall] 从 $CODEX_HOOKS 移除 agent_approver hook"
    HOOK="$HOOK" "$PYTHON" - "$CODEX_HOOKS" <<'PY'
import json, os, sys
path = sys.argv[1]
hook = os.environ["HOOK"]
try:
    data = json.load(open(path))
except Exception:
    sys.exit(0)
hooks = data.get("hooks", {})
for ev, groups in list(hooks.items()):
    kept = []
    for g in groups:
        hs = [h for h in g.get("hooks", []) if hook not in (h.get("command") or "")]
        if hs:
            g = dict(g); g["hooks"] = hs; kept.append(g)
    if kept:
        hooks[ev] = kept
    else:
        del hooks[ev]
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print("  done")
PY
  fi
  if [[ -f "$LAUNCH_PLIST" ]]; then
    launchctl unload "$LAUNCH_PLIST" 2>/dev/null || true
    rm -f "$LAUNCH_PLIST"
    echo "[uninstall] 已移除 launchd"
  fi
  echo "[uninstall] 完成 (配置 $CFG 保留)"
  exit 0
}

if [[ "${1:-}" == "--uninstall" ]]; then
  uninstall
fi

[[ -n "$PYTHON" ]] || { echo "需要 python3"; exit 1; }
chmod +x "$HOOK" "$RELAY" || true

# 0) BLE 依赖 (relay 蓝牙端需要 bleak; hook 只用标准库)
if "$PYTHON" -c "import bleak" 2>/dev/null; then
  echo "[deps] bleak 已安装"
else
  echo "[deps] 安装 bleak (relay 蓝牙端需要) ..."
  "$PYTHON" -m pip install --user bleak || \
    echo "[deps] !! bleak 安装失败, 请手动: $PYTHON -m pip install bleak"
fi

# 1) 默认配置
mkdir -p "$CFG_DIR"
if [[ ! -f "$CFG" ]]; then
  cp "$HERE/hooks/config.example.json" "$CFG"
  echo "[config] 写入默认配置 $CFG"
else
  echo "[config] 已存在 $CFG, 保留"
fi

# 2) 合并 hooks 到 ~/.cursor/hooks.json
mkdir -p "$HOME/.cursor"
[[ -f "$CURSOR_HOOKS" ]] && cp "$CURSOR_HOOKS" "$CURSOR_HOOKS.bak.$(date +%s)" && echo "[hooks] 备份旧 hooks.json"

HOOK="$HOOK" TEMPLATE="$HERE/hooks/hooks.template.json" "$PYTHON" - "$CURSOR_HOOKS" <<'PY'
import json, os, sys
path = sys.argv[1]
hook = os.environ["HOOK"]
tmpl = json.load(open(os.environ["TEMPLATE"]))

try:
    data = json.load(open(path))
except Exception:
    data = {"version": 1, "hooks": {}}
data.setdefault("version", 1)
data.setdefault("hooks", {})

for ev, arr in tmpl["hooks"].items():
    existing = data["hooks"].get(ev, [])
    # 先删掉本项目旧条目 (幂等), 再插新的
    existing = [h for h in existing if hook not in (h.get("command") or "")]
    for h in arr:
        h = dict(h)
        h["command"] = h["command"].replace("__HOOK__", hook)
        existing.append(h)
    data["hooks"][ev] = existing

json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print(f"[hooks] 已写入 {path}")
for ev in tmpl["hooks"]:
    print(f"  + {ev}")
PY

# 2b) 合并 hooks 到 ~/.claude/settings.json (Claude Code; 装了才弄)
if [[ -d "$HOME/.claude" ]] || command -v claude >/dev/null 2>&1; then
  mkdir -p "$HOME/.claude"
  [[ -f "$CLAUDE_SETTINGS" ]] && cp "$CLAUDE_SETTINGS" "$CLAUDE_SETTINGS.bak.$(date +%s)" && echo "[claude] 备份旧 settings.json"
  HOOK="$HOOK" TEMPLATE="$HERE/hooks/claude_settings.template.json" "$PYTHON" - "$CLAUDE_SETTINGS" <<'PY'
import json, os, sys
path = sys.argv[1]
hook = os.environ["HOOK"]
tmpl = json.load(open(os.environ["TEMPLATE"]))
try:
    data = json.load(open(path))
except Exception:
    data = {}
hooks = data.setdefault("hooks", {})
for ev, groups in tmpl["hooks"].items():
    existing = hooks.get(ev, [])
    cleaned = []
    for g in existing:                       # 删掉本项目旧条目 (幂等)
        hs = [h for h in g.get("hooks", []) if hook not in (h.get("command") or "")]
        if hs:
            g = dict(g); g["hooks"] = hs; cleaned.append(g)
    for g in groups:                         # 加新的
        g = json.loads(json.dumps(g))
        for h in g.get("hooks", []):
            h["command"] = h["command"].replace("__HOOK__", hook)
        cleaned.append(g)
    hooks[ev] = cleaned
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print(f"[claude] 已写入 {path}")
for ev in tmpl["hooks"]:
    print(f"  + {ev}")
PY
else
  echo "[claude] 未发现 ~/.claude, 跳过 Claude Code hooks (装了 claude 后重跑本脚本即可)"
fi

# 2c) 合并 hooks 到 ~/.codex/hooks.json (Codex; 装了才弄)
if [[ -d "$HOME/.codex" ]] || command -v codex >/dev/null 2>&1; then
  mkdir -p "$HOME/.codex"
  [[ -f "$CODEX_HOOKS" ]] && cp "$CODEX_HOOKS" "$CODEX_HOOKS.bak.$(date +%s)" && echo "[codex] 备份旧 hooks.json"
  HOOK="$HOOK" TEMPLATE="$HERE/hooks/codex_hooks.template.json" "$PYTHON" - "$CODEX_HOOKS" <<'PY'
import json, os, sys
path = sys.argv[1]
hook = os.environ["HOOK"]
tmpl = json.load(open(os.environ["TEMPLATE"]))
try:
    data = json.load(open(path))
except Exception:
    data = {}
hooks = data.setdefault("hooks", {})
for ev, groups in tmpl["hooks"].items():
    existing = hooks.get(ev, [])
    cleaned = []
    for g in existing:                       # 删掉本项目旧条目 (幂等)
        hs = [h for h in g.get("hooks", []) if hook not in (h.get("command") or "")]
        if hs:
            g = dict(g); g["hooks"] = hs; cleaned.append(g)
    for g in groups:                         # 加新的
        g = json.loads(json.dumps(g))
        for h in g.get("hooks", []):
            h["command"] = h["command"].replace("__HOOK__", hook)
        cleaned.append(g)
    hooks[ev] = cleaned
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print(f"[codex] 已写入 {path}")
for ev in tmpl["hooks"]:
    print(f"  + {ev}")
PY
  echo "[codex] 注意: Codex 首次需在 CLI 里跑 /hooks 审核并信任这些 hook"
else
  echo "[codex] 未发现 ~/.codex, 跳过 Codex hooks (装了 codex 后重跑本脚本即可)"
fi

# 3) (可选) launchd 自启
if [[ "${1:-}" == "--launchd" ]]; then
  mkdir -p "$HOME/Library/LaunchAgents"
  sed -e "s#__PYTHON__#$PYTHON#g" \
      -e "s#__RELAY__#$RELAY#g" \
      -e "s#__LOG__#$LOG#g" \
      "$HERE/relay/com.agentapprover.relay.plist" > "$LAUNCH_PLIST"
  launchctl unload "$LAUNCH_PLIST" 2>/dev/null || true
  launchctl load "$LAUNCH_PLIST"
  echo "[launchd] 已加载, relay 会开机自启 (日志: $LOG)"
fi

echo
echo "安装完成. 下一步:"
echo "  1) 给 StickS3 烧固件:  $HERE/firmware/flash.sh"
echo "     (它会广播为 BLE 设备 'AgentApprover')"
echo "  2) 启动 relay (若没用 --launchd):  $PYTHON $RELAY"
echo "     首次会通过蓝牙连接并配对 StickS3 (系统可能弹一次配对确认)."
echo "  3) 重启 Cursor 让 hooks 生效 (设置 -> Hooks 里能看到)"
echo "     Claude Code: 新开一个会话即生效 (hooks 写在 ~/.claude/settings.json)"
echo "     Codex: 新开会话, 首次需在 CLI 跑 /hooks 信任 (hooks 写在 ~/.codex/hooks.json)"
echo
echo "注意: relay 走蓝牙, 首次运行 macOS 可能要你授权 '蓝牙' 权限"
echo "      (系统设置 -> 隐私与安全性 -> 蓝牙). 用 --launchd 自启时尤其注意."
