"""把 assets/ 下的角色图片转成 RGB565 数组，生成 src/images.cpp。

用法：uv run python tools/convert_images.py

生成的文件提交进 git，编译时不需要 Pillow。数组按 4 字节对齐（ESP32 按 16 位读未对齐的地址会出错），
每个值就是一个 RGB565 像素（高 5 位红、中 6 位绿、低 5 位蓝），绘制时按 lgfx::rgb565_t 传给 pushImage。
"""

from __future__ import annotations

import pathlib
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "images.cpp"
WIDTH, HEIGHT = 336, 261  # 和 include/config.h 里的 kCharW / kCharH 一致
IMAGES = [
    ("kWhaleEating", "assets/whale_eating_336x261.png"),  # HOME
    ("kWhaleAsleepDim", "assets/whale_asleep_dim_336x261.png"),  # STANDBY，已预先调暗到 70%
]
PER_LINE = 12


def to_rgb565(r: int, g: int, b: int) -> int:
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def main() -> int:
    parts = [
        "// 由 tools/convert_images.py 从 assets/ 下的 PNG 生成，不要手改。",
        "// 每个值是一个 RGB565 像素，绘制时按 lgfx::rgb565_t 传给 pushImage。",
        "// 角色图片不适用仓库的 MIT 许可，见 assets/LICENSE.md。",
        '#include "images.h"',
        "",
        "namespace img {",
    ]
    for name, rel in IMAGES:
        im = Image.open(ROOT / rel).convert("RGB")
        if im.size != (WIDTH, HEIGHT):
            print(f"{rel} 的尺寸是 {im.size}，应该是 {(WIDTH, HEIGHT)}", file=sys.stderr)
            return 1
        raw = im.tobytes()  # 每像素 R、G、B 三个字节
        pixels = [to_rgb565(raw[i], raw[i + 1], raw[i + 2]) for i in range(0, len(raw), 3)]
        parts.append("")
        parts.append(f"// {rel}")
        parts.append(f"alignas(4) const uint16_t {name}[{WIDTH} * {HEIGHT}] = {{")
        for i in range(0, len(pixels), PER_LINE):
            parts.append("    " + ", ".join(f"0x{v:04X}" for v in pixels[i : i + PER_LINE]) + ",")
        parts.append("};")
        print(f"{rel} -> {name}（{len(pixels)} 像素）")
    parts.append("")
    parts.append("}  // namespace img")
    parts.append("")
    OUT.write_text("\n".join(parts), encoding="utf-8", newline="\n")
    print(f"已生成 {OUT.relative_to(ROOT)}（{OUT.stat().st_size // 1024} KB）")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
