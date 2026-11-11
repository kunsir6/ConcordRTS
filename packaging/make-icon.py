#!/usr/bin/env python3
"""生成 macOS 应用图标：1024 画布 + Big Sur 圆角 + 透明边距，交给 iconutil 出 .icns。

为什么需要这个脚本（而不是 sips 缩几张图）：
  纯 RGB 的方块图（比如游戏封面）直接做 .icns，系统**不报错**但也不理它——
  IconServices 需要一个带 alpha 的、按 macOS 形体裁过的图，否则回落成空白占位图。
  实测：把封面原画按 10 个尺寸 sips 缩放后，`iconutil` 甚至直接报
  "Invalid Iconset"，而 Finder 里显示的是通用应用图标。

形体参数来自 Apple 的 macOS 图标网格（1024 画布）：
  画布 1024×1024，内容 824×824 居中（四边各留 100），圆角半径约 185。
  留白与圆角不是审美选择——Dock 里系统图标都按这个网格排，不遵循会明显偏大/偏方。

用法：
  make-icon.py --source art.png --out-dir build/app --name "红色警戒2 尤里的复仇"
  make-icon.py --source art.png --out-dir out --name App --no-mask   # 图自带 alpha 与外形
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("make-icon: 需要 Pillow（python3 -m pip install pillow）")

# 十帧标准文件名（iconutil 只认这套命名，多一个少一个都可能被拒）
ICONSET_FRAMES = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]


def cover_fit(image: Image.Image, box: int) -> Image.Image:
    """等比铺满 box×box 并居中裁切。不拉伸——封面被拉扁比留白更难看。"""
    width, height = image.size
    scale = max(box / width, box / height)
    resized = image.resize(
        (max(box, round(width * scale)), max(box, round(height * scale))),
        Image.LANCZOS,
    )
    left = (resized.width - box) // 2
    top = (resized.height - box) // 2
    return resized.crop((left, top, left + box, top + box))


def build_canvas(source: Image.Image, canvas: int, content: int, radius: int) -> Image.Image:
    margin = (canvas - content) // 2
    art = cover_fit(source.convert("RGB"), content).convert("RGBA")

    mask = Image.new("L", (content, content), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, content - 1, content - 1), radius=radius, fill=255
    )

    # 阴影：用同一个圆角形状模糊后垫底。Big Sur 之后的图标阴影是烘进图里的，
    # 系统不会再补一层，没有它会显得"贴"在 Dock 上而不是浮着。
    shadow = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    shadow_shape = Image.new("RGBA", (content, content), (0, 0, 0, 0))
    shadow_shape.paste((0, 0, 0, 90), (0, 0, content, content), mask)
    shadow.alpha_composite(shadow_shape, (margin, margin + max(4, canvas // 128)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(canvas / 64))

    plate = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    plate.alpha_composite(shadow)
    plate.paste(art, (margin, margin), mask)

    # 顶部内高光：一条 1px、低透明度的白边，让小尺寸下圆角不至于糊成一坨。
    edge = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle(
        (margin, margin, margin + content - 1, margin + content - 1),
        radius=radius, outline=(255, 255, 255, 38), width=max(1, canvas // 512),
    )
    plate.alpha_composite(edge)
    return plate


def build_raw(source: Image.Image, canvas: int) -> Image.Image:
    """源图已经自带 alpha 与形体时，只做等比缩放，不再套圆角。"""
    image = source.convert("RGBA")
    scale = canvas / max(image.size)
    if scale != 1:
        image = image.resize(
            (max(1, round(image.width * scale)), max(1, round(image.height * scale))),
            Image.LANCZOS,
        )
    plate = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    plate.alpha_composite(image, ((canvas - image.width) // 2, (canvas - image.height) // 2))
    return plate


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 macOS .icns（圆角+透明）")
    parser.add_argument("--source", required=True, help="源图（任意尺寸 PNG）")
    parser.add_argument("--out-dir", required=True, help=".icns 与 .iconset 的输出目录")
    parser.add_argument("--name", required=True, help="输出文件名（不带扩展名）")
    parser.add_argument("--canvas", type=int, default=1024)
    parser.add_argument("--content", type=int, default=824, help="圆角内容边长（Big Sur 网格为 824）")
    parser.add_argument("--radius", type=int, default=185, help="圆角半径（824 内容对应约 185）")
    parser.add_argument("--no-mask", action="store_true", help="源图自带 alpha/外形，不套圆角")
    args = parser.parse_args()

    source_path = Path(args.source)
    if not source_path.is_file():
        sys.exit(f"make-icon: 源图不存在: {source_path}")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    source = Image.open(source_path)
    if args.no_mask:
        canvas_image = build_raw(source, args.canvas)
    else:
        canvas_image = build_canvas(source, args.canvas, args.content, args.radius)

    iconset = out_dir / "icon.iconset"
    # iconutil 要求目录以 .iconset 结尾；重建以丢弃上一次的残留帧。
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)

    for filename, pixels in ICONSET_FRAMES:
        frame = canvas_image if pixels == args.canvas else canvas_image.resize(
            (pixels, pixels), Image.LANCZOS
        )
        frame.save(iconset / filename, format="PNG")

    icns_path = out_dir / f"{args.name}.icns"
    if icns_path.exists():
        icns_path.unlink()
    result = subprocess.run(
        ["iconutil", "-c", "icns", str(iconset), "-o", str(icns_path)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.exit(f"make-icon: iconutil 失败: {result.stderr.strip() or result.stdout.strip()}")

    print(f"icon: {icns_path} ({icns_path.stat().st_size} bytes, "
          f"{args.canvas}px canvas, alpha=yes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
