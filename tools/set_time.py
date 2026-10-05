"""把电脑当前时间和时区写进设备。USB 同步脚本 sync.py 每次也会先校时，只想校时的时候用这个。

用法：uv run python tools/set_time.py [--port <串口>]（不给就自动找，见 devport.py）

发给设备的命令是 "TIME <unix秒> <时区分钟>"（SPEC 第 8 节）：RTC 存 UTC，时区偏移单独保存。
设备要醒着（深睡眠时 USB 串口不在）。串口不能同时被 serial_log.py 占用；
日志脚本在跑时，改为往它的 --cmd 文件里追加这一行。
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
import time

import serial

import devport


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows 管道默认 cp1252，打印中文会报错
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="把电脑时间写进设备 RTC")
    ap.add_argument("--port", help="设备的串口，不给就自动找")
    args = ap.parse_args()
    port = devport.resolve(args.port)

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        print(f"打不开 {port}：{e}", file=sys.stderr)
        return 1

    tz_min = int(dt.datetime.now().astimezone().utcoffset().total_seconds() // 60)
    time.sleep(1 - time.time() % 1)  # 对齐到整秒再发
    unix = int(round(time.time()))
    ser.write(f"TIME {unix} {tz_min}\n".encode())

    deadline = time.time() + 3
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("OK TIME"):
            print(f"已校时：{dt.datetime.fromtimestamp(unix)}（UTC{tz_min / 60:+g}）")
            return 0
        if line.startswith("ERR"):
            print(f"设备返回错误：{line}", file=sys.stderr)
            return 1
    print("设备没有回应：确认设备醒着，串口没被其他程序占用。", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
