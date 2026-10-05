"""串口日志：自动重连，每行加上电脑时间写进文件。

用法：uv run python tools/serial_log.py --log logs/serial.log [--cmd logs/cmd.txt] [--port <串口>]

--cmd 指定的文件每追加一行，就把这一行发给设备（串口被本脚本占着时，用它给设备发命令）。
打开串口时不拉 DTR/RTS，不会复位设备。设备深睡眠或重启时串口会消失，脚本会自动重连。
--port 不给就按 USB 厂商号自动找（见 devport.py），每次重连都重新找。
"""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import sys
import time

import serial

import devport


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def main() -> int:
    ap = argparse.ArgumentParser(description="串口日志（自动重连）")
    ap.add_argument("--port", help="设备的串口，不给就自动找")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", required=True, type=pathlib.Path)
    ap.add_argument("--cmd", type=pathlib.Path)
    args = ap.parse_args()

    args.log.parent.mkdir(parents=True, exist_ok=True)
    log = args.log.open("a", encoding="utf-8", buffering=1)

    def write(line: str) -> None:
        stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        log.write(f"{stamp} {line}\n")

    cmd_pos = 0
    if args.cmd:
        args.cmd.parent.mkdir(parents=True, exist_ok=True)
        args.cmd.touch()
        cmd_pos = args.cmd.stat().st_size

    ser: serial.Serial | None = None
    pending = b""
    while True:
        if ser is None:
            port = args.port or devport.single()
            if port is None:
                time.sleep(0.5)
                continue
            try:
                ser = open_port(port, args.baud)
                write("# 已连接")
            except serial.SerialException:
                time.sleep(0.5)
                continue
        try:
            data = ser.read(4096)
            if data:
                pending += data
                *lines, pending = pending.split(b"\n")
                for raw in lines:
                    write(raw.rstrip(b"\r").decode("utf-8", errors="replace"))
            if args.cmd:
                size = args.cmd.stat().st_size
                if size < cmd_pos:
                    cmd_pos = 0
                if size > cmd_pos:
                    with args.cmd.open("rb") as f:
                        f.seek(cmd_pos)
                        chunk = f.read()
                    cmd_pos = size
                    for cmd in chunk.decode("utf-8").splitlines():
                        if cmd.strip():
                            ser.write((cmd.strip() + "\n").encode())
                            write(f"> {cmd.strip()}")
        except (serial.SerialException, OSError):
            write("# 连接断开")
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            pending = b""
            time.sleep(0.5)


if __name__ == "__main__":
    sys.exit(main())
