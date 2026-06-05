#!/usr/bin/env python3
# =============================================================================
# ble.py - relay 的蓝牙 (BLE central) 端. 用 bleak 连到 StickS3 (BLE 外设,
# Nordic UART Service), 把待审批/活动写过去, 把按键决定收回来.
#
# 协议: 两个方向都是 "换行分隔的 JSON", 经 NUS 的两个特征:
#   RX (write,  central->peripheral): relay 写
#        {"t":"reset"} | {"t":"agent",...} | {"t":"agent_del","id"} |
#        {"t":"approval",...} | {"t":"cancel","id"}
#   TX (notify, peripheral->central): stick 发 {"t":"decide","id","decision"}
# 本文件对消息内容透明, 只负责搬运 outbox -> stick 和 decide -> state.
#
# 需要 bleak: pip3 install bleak  (macOS 走 CoreBluetooth).
# =============================================================================
from __future__ import annotations

import asyncio
import json
import queue
import threading
from typing import Callable, Optional

try:
    from bleak import BleakClient, BleakScanner
except Exception:  # noqa: BLE001
    BleakClient = None  # type: ignore
    BleakScanner = None  # type: ignore

# Nordic UART Service
NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write  (relay -> stick)
NUS_TX  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify (stick -> relay)


class BleWorker(threading.Thread):
    def __init__(self, state, name: str = "AgentApprover",
                 address: Optional[str] = None,
                 log: Callable[[str], None] = print) -> None:
        super().__init__(daemon=True)
        self.state = state
        self.name = name
        self.address = address
        self.log = log
        self._stop = False
        self._rxbuf = b""

    def stop(self) -> None:
        self._stop = True

    def run(self) -> None:
        if BleakClient is None:
            self.log("bleak 不可用, BLE worker 退出")
            return
        try:
            asyncio.run(self._main())
        except Exception as e:  # noqa: BLE001
            self.log(f"ble worker crashed: {e}")

    async def _main(self) -> None:
        backoff = 2.0
        while not self._stop:
            try:
                dev = await self._find()
                if not dev:
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 1.5, 15.0)
                    continue
                backoff = 2.0
                await self._session(dev)
            except Exception as e:  # noqa: BLE001
                self.log(f"ble session error: {e}")
            finally:
                self.state.on_ble_down()
            await asyncio.sleep(2.0)

    async def _find(self):
        if self.address:
            return self.address
        self.log(f"BLE 扫描 '{self.name}' ...")
        try:
            return await BleakScanner.find_device_by_name(self.name, timeout=8.0)
        except Exception as e:  # noqa: BLE001
            self.log(f"ble scan error: {e}")
            return None

    def _on_notify(self, _char, data: bytearray) -> None:
        self._rxbuf += bytes(data)
        while b"\n" in self._rxbuf:
            line, self._rxbuf = self._rxbuf.split(b"\n", 1)
            self._on_line(line.decode("utf-8", "replace"))

    def _on_line(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        try:
            msg = json.loads(line)
        except Exception:  # noqa: BLE001
            return
        if msg.get("t") == "decide":
            self.state.decide(str(msg.get("id") or ""), str(msg.get("decision") or ""))

    async def _session(self, dev) -> None:
        self._rxbuf = b""
        async with BleakClient(dev) as client:
            self.log(f"BLE 已连接 {getattr(dev, 'address', dev)}")
            # macOS 上访问加密特征会自动配对; pair() 多半是 no-op, 失败无所谓.
            try:
                await client.pair()
            except Exception:  # noqa: BLE001
                pass
            await client.start_notify(NUS_TX, self._on_notify)
            self.state.on_ble_up()
            try:
                while client.is_connected and not self._stop:
                    drained = await self._drain_outbox(client)
                    if not drained:
                        await asyncio.sleep(0.05)
            finally:
                try:
                    await client.stop_notify(NUS_TX)
                except Exception:  # noqa: BLE001
                    pass

    @staticmethod
    def _chunk_size(client) -> int:
        # 单次 BLE 写不能超过 ATT_MTU-3 (也受特征最大长度限制), 否则 CoreBluetooth
        # 报 "value's length is invalid". 长消息 (审批) 切片发, 固件按换行重新拼.
        mtu = 0
        try:
            mtu = int(getattr(client, "mtu_size", 0) or 0)
        except Exception:  # noqa: BLE001
            mtu = 0
        return max(20, mtu - 3) if mtu >= 23 else 20

    async def _drain_outbox(self, client) -> bool:
        sent = False
        chunk = self._chunk_size(client)
        while True:
            try:
                msg = self.state.outbox.get_nowait()
            except queue.Empty:
                break
            data = (json.dumps(msg, ensure_ascii=False) + "\n").encode("utf-8")
            try:
                for i in range(0, len(data), chunk):
                    await client.write_gatt_char(NUS_RX, data[i:i + chunk], response=True)
                sent = True
            except Exception as e:  # noqa: BLE001
                self.log(f"ble write error: {e}")
                break
        return sent
