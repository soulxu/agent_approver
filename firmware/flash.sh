#!/usr/bin/env bash
# agent_approver 编译 + 烧录脚本 (M5StickS3).
#
#   firmware/flash.sh            # 自动找端口, 编译并烧录
#   firmware/flash.sh build      # 只编译
#   firmware/flash.sh upload     # 只烧录
#   firmware/flash.sh monitor    # 只串口监视
#   firmware/flash.sh all        # 编译 + 烧录 + 监视
#   PORT=/dev/cu.usbmodem11201 firmware/flash.sh   # 强制指定端口
#
# 进入下载模式: 长按机身侧面 reset 键约 2 秒, 等绿色 LED 闪烁后松开.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"   # 仓库根 (有 acli + .arduino15)
SKETCH="$HERE"
ACLI="$ROOT/acli"
FQBN_BASE="m5stack:esp32:m5stack_sticks3"
FQBN="${FQBN_BASE}:FlashSize=8M,PartitionScheme=custom"
BAUD="${BAUD:-115200}"

if [[ ! -x "$ACLI" ]]; then
  echo "[flash] cannot find acli at $ACLI" >&2
  exit 1
fi

find_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  local p
  p="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
  if [[ -z "$p" ]]; then
    echo "[flash] no /dev/cu.usbmodem* found, plug in the StickS3 (hold side reset to enter download mode) or set PORT=" >&2
    exit 1
  fi
  echo "$p"
}

do_build()  { echo "[flash] compile $SKETCH"; "$ACLI" compile --fqbn "$FQBN" "$SKETCH"; }
do_upload() { echo "[flash] upload -> $1"; "$ACLI" upload --fqbn "$FQBN" -p "$1" "$SKETCH"; }
do_monitor(){ echo "[flash] monitor $1 @ $BAUD"; "$ACLI" monitor --port "$1" -c "baudrate=$BAUD"; }

cmd="${1:-flash}"
case "$cmd" in
  build)   do_build ;;
  upload)  do_upload "$(find_port)" ;;
  monitor) do_monitor "$(find_port)" ;;
  all)     port="$(find_port)"; do_build; do_upload "$port"; do_monitor "$port" ;;
  flash|"") port="$(find_port)"; do_build; do_upload "$port" ;;
  *) echo "usage: $0 [build|upload|monitor|all|flash]" >&2; exit 2 ;;
esac
