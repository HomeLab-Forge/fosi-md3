"""Render a 240x240 animated GIF for the MD3's round display (test card: sweep + label)."""

from __future__ import annotations

import argparse
import math

from PIL import Image, ImageDraw, ImageFont

SIZE = 240
FONT = "/System/Library/Fonts/Menlo.ttc"


def frame(i: int, n: int, title: str, subtitle: str) -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (6, 8, 10))
    d = ImageDraw.Draw(img)
    c = SIZE / 2
    # Concentric rings, matching the round panel's edge.
    for r, col in ((118, (40, 44, 48)), (96, (28, 32, 36)), (74, (28, 32, 36))):
        d.ellipse([c - r, c - r, c + r, c + r], outline=col, width=1)
    # Sweep with a fading tail.
    a0 = 2 * math.pi * i / n
    for k in range(24):
        a = a0 - k * 0.035
        shade = max(0, 255 - k * 11)
        x, y = c + 116 * math.cos(a), c + 116 * math.sin(a)
        d.line([c, c, x, y], fill=(255 * shade // 255, 96 * shade // 255, 24 * shade // 255), width=2)
    # Tick marks every 30 degrees.
    for t in range(12):
        a = 2 * math.pi * t / 12
        d.line([c + 106 * math.cos(a), c + 106 * math.sin(a),
                c + 118 * math.cos(a), c + 118 * math.sin(a)], fill=(200, 200, 200), width=2)
    big = ImageFont.truetype(FONT, 30)
    small = ImageFont.truetype(FONT, 13)
    d.rounded_rectangle([42, 92, 198, 152], radius=8, fill=(6, 8, 10), outline=(255, 96, 24), width=2)
    d.text((c, 112), title, font=big, fill=(240, 240, 240), anchor="mm")
    d.text((c, 140), subtitle, font=small, fill=(255, 96, 24), anchor="mm")
    d.text((c, 176), f"frame {i:02d}/{n}", font=small, fill=(140, 140, 140), anchor="mm")
    return img


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--title", default="OWNED")
    ap.add_argument("--subtitle", default="md3 // 2026-10-26")
    ap.add_argument("--frames", type=int, default=36)
    ap.add_argument("--ms", type=int, default=80)
    args = ap.parse_args()
    frames = [frame(i, args.frames, args.title, args.subtitle).quantize(colors=64, dither=Image.Dither.NONE)
              for i in range(args.frames)]
    frames[0].save(args.out, save_all=True, append_images=frames[1:], duration=args.ms, loop=0,
                   optimize=False, disposal=1)


if __name__ == "__main__":
    main()
