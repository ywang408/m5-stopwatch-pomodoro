"""把设计稿用的字体做成 M5GFX 能用的 VLW 字体，生成 src/fonts.h 和 src/fonts.cpp。

用法：uv run python tools/make_fonts.py [--cjk C:/Windows/Fonts/NotoSansSC-VF.ttf]

- 数字：IBM Plex Mono ExtraLight（fonts/IBMPlexMono-ExtraLight.ttf），SPEC 5.3 的各个字号；
- D 的大数字：Instrument Serif（fonts/InstrumentSerif-Regular.ttf）150px；
- 中文：Noto Sans SC Regular，12–16px，只收 tools/charset.txt 字表里的字。Windows 自带的是可变字重版，这里取 wght=400。

每个字体只收需要的字，灰度位图（每像素一字节）。VLW 格式见 M5GFX 的 VLWfont::loadFont：
文件头 6 个大端 int32（字数、版本、字号、0、ascent、descent），每个字 7 个大端 int32
（unicode、高、宽、前进宽度、基线以上的高度、左边距、0），按 unicode 从小到大排，后面接所有字形的位图。
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys

import freetype

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLEX = ROOT / "fonts" / "IBMPlexMono-ExtraLight.ttf"
SERIF = ROOT / "fonts" / "InstrumentSerif-Regular.ttf"
DEFAULT_CJK = pathlib.Path("C:/Windows/Fonts/NotoSansSC-VF.ttf")
OUT_H = ROOT / "src" / "fonts.h"
OUT_CPP = ROOT / "src" / "fonts.cpp"

DIGITS = "0123456789:+-%"  # % 是充电画面的百分比用的（SPEC 12.5）
SERIF_DIGITS = "0123456789"
CJK_EXTRA = "0123456789:+×/·？-%USB "  # 字表之外固定加上数字、:+×/·？-%、USB 和空格
CJK_WEIGHT = 400

# (C++ 名字, 字体, 像素大小, 字表, 用在哪)
PLEX_SIZES = [
    (92, "A、G 的倒计时，K 的超时"),
    (80, "充电画面的百分比"),
    (76, "E、J 的倒计时"),
    (72, "F、K2 的分钟数"),
    (64, "HOME 的时间、L 的超时"),
    (48, "I 的“+25”"),
    (44, "H、K3 的时间"),
    (13, "D 的刻度数字"),
]
CJK_SIZES = [
    (12, "D 的提示和电量警告、F / K2 的“分钟”、电池图标的百分比"),
    (13, "“第 N / 4 个番茄”、HOME 的日期、“松手即取消”"),
    (14, "E / J 的标签、I 的“分钟”、“计入本次番茄？”"),
    (16, "D 的“分钟”和标签名、HOME 的“× N”、L 的“计入”“不计”"),
]


def charset_chars(path: pathlib.Path) -> str:
    """字表文件里的字：去掉 # 开头的注释行和所有空白。"""
    lines = [l for l in path.read_text(encoding="utf-8").splitlines() if not l.lstrip().startswith("#")]
    return "".join("".join(lines).split())


def check_coverage(chars: str) -> None:
    """界面代码和常量表（标签名在 config.h）里用到的非 ASCII 字都要在字表里，缺了就报错（字表要跟着补）。"""
    used = set()
    for path in [ROOT / "src" / "ui.cpp", ROOT / "include" / "config.h"]:
        for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', path.read_text(encoding="utf-8")):
            used.update(c for c in literal if ord(c) > 0x7F)
    missing = sorted(used - set(chars))
    if missing:
        sys.exit(f"界面用到了字表里没有的字：{''.join(missing)}（先补进 tools/charset.txt）")


def render_font(path: pathlib.Path, px: int, chars: str, weight: int | None = None) -> tuple[bytes, int, int, int]:
    """返回（VLW 内容，行框中心到基线的距离，ascent，descent）。"""
    face = freetype.Face(str(path))
    if weight is not None:
        face.set_var_design_coords((float(weight),))
    face.set_pixel_sizes(0, px)
    ascent = round(face.size.ascender / 64)
    descent = round(-face.size.descender / 64)

    entries = []
    bitmaps = []
    for ch in sorted(set(chars), key=ord):
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
        g = face.glyph
        bm = g.bitmap
        w, h, pitch = bm.width, bm.rows, bm.pitch
        buf = bytes(bm.buffer)
        pixels = b"".join(buf[row * pitch: row * pitch + w] for row in range(h))
        advance = round(g.advance.x / 64)
        if not (0 <= w <= 255 and 0 <= advance <= 255 and -128 <= g.bitmap_left <= 127):
            sys.exit(f"{path.name} {px}px 的“{ch}”超出 VLW 的范围（宽 {w}、前进 {advance}、左边距 {g.bitmap_left}）")
        entries.append((ord(ch), h, w, advance, g.bitmap_top, g.bitmap_left, 0))
        bitmaps.append(pixels)

    out = bytearray(struct.pack(">6i", len(entries), 11, px, 0, ascent, descent))
    for e in entries:
        out += struct.pack(">7i", *e)
    for b in bitmaps:
        out += b
    # 浏览器排一行字时，ascent + descent 这块内容区在行框里上下居中，所以基线在行框中心往下 (ascent − descent) / 2
    baseline = round((face.size.ascender + face.size.descender) / 64 / 2)
    return bytes(out), baseline, ascent, descent


def c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    return "\n".join(lines)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="生成 VLW 字体")
    ap.add_argument("--cjk", type=pathlib.Path, default=DEFAULT_CJK, help="Noto Sans SC 字体文件")
    args = ap.parse_args()

    cjk_chars = charset_chars(ROOT / "tools" / "charset.txt") + CJK_EXTRA
    check_coverage(cjk_chars)

    fonts = []  # (名字, 说明, 数据, (基线, ascent, descent))
    for px, note in PLEX_SIZES:
        data, *metrics = render_font(PLEX, px, DIGITS)
        fonts.append((f"kPlex{px}", f"IBM Plex Mono ExtraLight {px}px：{note}", data, metrics))
    data, *metrics = render_font(SERIF, 150, SERIF_DIGITS)
    fonts.append(("kSerif150", "Instrument Serif 150px：D 的大数字", data, metrics))
    for px, note in CJK_SIZES:
        data, *metrics = render_font(args.cjk, px, cjk_chars, CJK_WEIGHT)
        fonts.append((f"kSans{px}", f"Noto Sans SC {px}px：{note}", data, metrics))

    header = [
        "// 由 tools/make_fonts.py 生成，不要手改。字表见 tools/charset.txt。",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace fontdata {",
        "",
        "struct Font {",
        "  const uint8_t* vlw;  // VLW 字体文件的内容",
        "  size_t size;",
        "  int baseline;        // 行框中心到基线的距离（px，向下为正），和浏览器排一行字的方式一样",
        "  int ascent;          // 基线以上、以下各占多少（px），算文字占的矩形用",
        "  int descent;",
        "};",
        "",
    ]
    for name, note, _, _ in fonts:
        header.append(f"extern const Font {name};  // {note}")
    header += ["", "}  // namespace fontdata", ""]

    cpp = [
        "// 由 tools/make_fonts.py 生成，不要手改。",
        '#include "fonts.h"',
        "",
        "namespace fontdata {",
        "",
        "namespace {",
        "",
    ]
    for name, note, data, _ in fonts:
        cpp.append(f"// {note}")
        cpp.append(f"alignas(4) const uint8_t {name}Data[] = {{")
        cpp.append(c_array(data))
        cpp.append("};")
        cpp.append("")
    cpp += ["}  // namespace", ""]
    for name, _, _, (base, asc, desc) in fonts:
        cpp.append(f"const Font {name} = {{{name}Data, sizeof({name}Data), {base}, {asc}, {desc}}};")
    cpp += ["", "}  // namespace fontdata", ""]

    # Windows 上也写 LF，和仓库里的一致（.gitattributes 是 eol=lf）
    OUT_H.write_text("\n".join(header), encoding="utf-8", newline="\n")
    OUT_CPP.write_text("\n".join(cpp), encoding="utf-8", newline="\n")
    total = sum(len(d) for _, _, d, _ in fonts)
    for name, note, data, (base, asc, desc) in fonts:
        print(f"{name:10s} {len(data):7d} 字节  基线 {base:+d}px  上 {asc} 下 {desc}  {note}")
    print(f"共 {total} 字节，中文 {len(set(cjk_chars))} 个字（含数字和符号）。已写 {OUT_H.name}、{OUT_CPP.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
