"""找设备的串口：按 USB 厂商号认 ESP32-S3（Espressif，0x303A）。

sync.py、set_time.py、serial_log.py 没给 --port 时用它。电脑上同时插着好几块 ESP32 时，要自己用 --port 指定。
设备深睡眠时 USB 串口不在，先按一下按键叫醒。
"""

from __future__ import annotations

import serial.tools.list_ports

ESPRESSIF_VID = 0x303A


def candidates() -> list[str]:
    return sorted(p.device for p in serial.tools.list_ports.comports() if p.vid == ESPRESSIF_VID)


def single() -> str | None:
    """正好找到一个时返回它，否则返回 None（给自动重连用，找不到就接着等）。"""
    found = candidates()
    return found[0] if len(found) == 1 else None


def resolve(port: str | None) -> str:
    """给了 --port 就用它；没给就自动找，找不到或找到好几个时退出并说明。"""
    if port:
        return port
    found = candidates()
    if len(found) == 1:
        return found[0]
    if not found:
        raise SystemExit("没找到设备的串口。设备深睡眠时 USB 串口不在，先按一下按键叫醒；也可以用 --port 指定。")
    raise SystemExit(f"找到好几个 ESP32 的串口：{'、'.join(found)}。用 --port 指定一个。")
