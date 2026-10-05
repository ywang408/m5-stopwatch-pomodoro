"""USB 同步（SPEC 第 8 节）：校时，取回新记录，存到电脑上，最后告诉设备存到哪了。

用法：uv run python tools/sync.py [--port <串口>] [--out data]（串口不给就自动找，见 devport.py）

每次同步：
1. TIME：把电脑的时间和时区写进设备。
2. RECORDS <after_seq>：取回行号 seq 比本地已有的更大的记录，原样追加到 <out>/records.jsonl。
3. 按番茄 id 合并（同一个番茄可能有好几行，以最后一行为准），重新生成 <out>/records.csv，
   开始、结束时间换成电脑的本地时间，标签写中文（没有标签的旧记录算学习）。带 "test": true 的是
   调试版加速时记的测试记录：records.jsonl 里照样留着，CSV 里不放。
4. ACK <max_seq>：告诉设备已经存到哪一行。

设备要醒着（深睡眠时 USB 串口不在），串口也不能被 serial_log.py 占着。
打开串口时不拉 DTR/RTS，不会复位设备。
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import pathlib
import sys
import time

import serial

import devport

REPLY_TIMEOUT_S = 3.0      # 等 OK TIME / OK ACK
RECORDS_IDLE_TIMEOUT_S = 5.0  # 回传记录时，这么久没有新的一行就算失败
MIN_VALID_TS = 1_767_225_600  # 2026-01-01：早于它的时间戳是设备没校时的时候记的，不可信

CSV_FIELDS = [
    "id", "start_local", "end_local", "tag", "planned_min", "focus_min", "paused_min", "pause_count",
    "overtime_min", "overtime_included", "end", "round_id", "index_in_round", "start_ts", "end_ts",
]
# 标签（SPEC 第 11 节）：CSV 里写中文；阶段 8 以前的记录没有 tag，算学习
TAG_NAMES = {"study": "学习", "work": "工作", "research": "研究"}


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def read_line(ser: serial.Serial) -> str | None:
    raw = ser.readline()
    if not raw:
        return None
    return raw.decode("utf-8", errors="replace").strip()


def wait_reply(ser: serial.Serial, prefix: str) -> str:
    """等一行以 prefix 开头的回复；日志行（EVT ...）跳过。"""
    deadline = time.time() + REPLY_TIMEOUT_S
    while time.time() < deadline:
        line = read_line(ser)
        if line is None:
            continue
        if line.startswith(prefix):
            return line
        if line.startswith("ERR"):
            raise RuntimeError(f"设备返回错误：{line}")
    raise RuntimeError(f"设备没有回 {prefix}：确认设备醒着，串口没被其他程序占用。")


def send(ser: serial.Serial, text: str) -> None:
    ser.write((text + "\n").encode())


def local_max_seq(jsonl: pathlib.Path) -> int:
    best = 0
    if jsonl.exists():
        for line in jsonl.read_text(encoding="utf-8").splitlines():
            if line.strip():
                best = max(best, int(json.loads(line)["seq"]))
    return best


def fetch_records(ser: serial.Serial, after_seq: int) -> tuple[list[str], int]:
    """发 RECORDS，收记录行直到 END。返回（记录行，设备上最大的 seq）。"""
    send(ser, f"RECORDS {after_seq}")
    lines: list[str] = []
    last_progress = time.time()
    while time.time() - last_progress < RECORDS_IDLE_TIMEOUT_S:
        line = read_line(ser)
        if line is None:
            continue
        if line.startswith("{"):
            json.loads(line)  # 坏行就直接报错，不往下写
            lines.append(line)
            last_progress = time.time()
        elif line.startswith("END"):
            parts = line.split()
            count = int(parts[1])
            device_max = int(parts[2]) if len(parts) > 2 else -1
            if count != len(lines):
                raise RuntimeError(f"设备说发了 {count} 行，实际收到 {len(lines)} 行，这次不保存。")
            return lines, device_max
        elif line.startswith("ERR"):
            raise RuntimeError(f"设备返回错误：{line}")
    raise RuntimeError("设备回传记录时断了（超时），这次不保存。")


def to_local(ts: int) -> str:
    if ts < MIN_VALID_TS:
        return ""  # 设备当时没校时，时间不可信
    return dt.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")


def write_csv(jsonl: pathlib.Path, csv_path: pathlib.Path) -> tuple[int, int, int]:
    """按 id 合并（以 seq 最大的一行为准），测试记录不要，按开始时间排序写 CSV。
    返回（番茄数，时间不可信的条数，测试记录数）。"""
    latest: dict[int, dict] = {}
    for line in jsonl.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        old = latest.get(rec["id"])
        if old is None or rec["seq"] > old["seq"]:
            latest[rec["id"]] = rec
    tests = sum(1 for r in latest.values() if r.get("test"))
    records = sorted((r for r in latest.values() if not r.get("test")), key=lambda r: (r["start_ts"], r["id"]))
    untrusted = sum(1 for r in records if r["start_ts"] < MIN_VALID_TS)

    with csv_path.open("w", encoding="utf-8-sig", newline="") as f:  # 带 BOM，Excel 直接打开不乱码
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        for r in records:
            w.writerow({
                "id": r["id"],
                "start_local": to_local(r["start_ts"]),
                "end_local": to_local(r["end_ts"]),
                "tag": TAG_NAMES.get(r.get("tag", "study"), r.get("tag")),
                "planned_min": round(r["planned_s"] / 60, 1),
                "focus_min": round(r["focus_s"] / 60, 1),
                "paused_min": round(r["paused_s"] / 60, 1),
                "pause_count": r["pause_count"],
                "overtime_min": round(r["overtime_s"] / 60, 1),
                "overtime_included": r["overtime_included"],
                "end": r["end"],
                "round_id": r["round_id"],
                "index_in_round": r["index_in_round"],
                "start_ts": r["start_ts"],
                "end_ts": r["end_ts"],
            })
    return len(records), untrusted, tests


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows 管道默认 cp1252，打印中文会报错
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="USB 同步番茄钟记录")
    ap.add_argument("--port", help="设备的串口，不给就自动找")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("data"), help="记录存放的目录")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    jsonl = args.out / "records.jsonl"
    csv_path = args.out / "records.csv"

    port = devport.resolve(args.port)
    try:
        ser = open_port(port)
    except serial.SerialException as e:
        print(f"打不开 {port}：{e}", file=sys.stderr)
        return 1

    try:
        ser.reset_input_buffer()
        tz_min = int(dt.datetime.now().astimezone().utcoffset().total_seconds() // 60)
        time.sleep(1 - time.time() % 1)  # 对齐到整秒再发
        unix = int(round(time.time()))
        send(ser, f"TIME {unix} {tz_min}")
        wait_reply(ser, "OK TIME")
        print(f"已校时：{dt.datetime.fromtimestamp(unix)}（UTC{tz_min / 60:+g}）")

        after = local_max_seq(jsonl)
        lines, device_max = fetch_records(ser, after)
        if 0 <= device_max < after:
            print(f"注意：设备上的记录只到第 {device_max} 行，电脑上已经有到第 {after} 行。"
                  "设备的存储可能被清空过，新记录不会被取回，需要另外处理。", file=sys.stderr)

        if lines:
            lines.sort(key=lambda s: json.loads(s)["seq"])
            with jsonl.open("a", encoding="utf-8") as f:
                for line in lines:
                    f.write(line + "\n")
            max_seq = json.loads(lines[-1])["seq"]
            send(ser, f"ACK {max_seq}")
            wait_reply(ser, "OK ACK")

        total, untrusted, tests = write_csv(jsonl, csv_path) if jsonl.exists() else (0, 0, 0)
        print(f"取回 {len(lines)} 行新记录；电脑上一共 {total} 个番茄。")
        print(f"CSV：{csv_path.resolve()}")
        if untrusted:
            print(f"其中 {untrusted} 个番茄是设备没校时的时候记的，开始、结束时间留空。")
        if tests:
            print(f"另有 {tests} 个测试记录（调试版加速时记的），只留在 records.jsonl 里，不放进 CSV。")
        return 0
    except (RuntimeError, ValueError, KeyError, json.JSONDecodeError) as e:
        print(f"同步失败：{e}", file=sys.stderr)
        return 1
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
