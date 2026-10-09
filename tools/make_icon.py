#!/usr/bin/env python3
"""
Build OLAS.ico from a dark-on-white logo.

The source is a flat near-black silhouette on pure white. Turning that into a
transparent-background icon looks correct on a light taskbar and vanishes on a
dark one, so instead the artwork is drawn in white on a solid rounded-square
tile. The tile carries the identity at every theme, and the wave stays legible
at 16 px because it is the only thing on the canvas.

Usage: make_icon.py <source.jpg> <out.ico>
"""

import argparse
import sys
from PIL import Image, ImageDraw

# Sizes Windows asks for. 256 is Explorer's extra-large view and the alt-tab
# switcher; the rest are taskbar, title bar and notification area.
SIZES = [256, 128, 64, 48, 32, 24, 16]

# Source luminance thresholds. At or below INK is fully part of the artwork;
# at or above PAPER is fully background; between is the antialiased edge and
# is kept as a ramp. Dropping that ramp is what makes small sizes look jagged.
INK = 80
PAPER = 240

# Tile colour. Deep teal: reads as water, holds white artwork at good contrast,
# and is dark enough to look deliberate rather than washed out on a light bar.
TILE_RGB = (23, 74, 82)      # #174A52
ART_RGB = (255, 255, 255)    # the wave, drawn white on the tile


def artwork_mask(gray: Image.Image) -> Image.Image:
    """Luminance -> coverage, where 255 means 'this pixel is the wave'."""
    span = float(PAPER - INK)
    lut = []
    for v in range(256):
        if v <= INK:
            a = 255
        elif v >= PAPER:
            a = 0
        else:
            a = int(round(255.0 * (PAPER - v) / span))
        lut.append(a)
    return gray.point(lut)


def rounded_tile(size: int) -> Image.Image:
    """A rounded square, antialiased, on a transparent canvas."""
    # Draw at 4x then downsample so the corners are smooth.
    ss = 4
    big = Image.new("RGBA", (size * ss, size * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    radius = int(size * ss * 0.22)          # Windows 11-ish corner
    d.rounded_rectangle(
        [0, 0, size * ss - 1, size * ss - 1],
        radius=radius, fill=TILE_RGB + (255,))
    return big.resize((size, size), Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    args = ap.parse_args()

    gray = Image.open(args.src).convert("L")
    mask = artwork_mask(gray)

    # Tight-crop to the artwork, so the tile is not mostly empty margin.
    box = mask.point(lambda p: 255 if p > 8 else 0).getbbox()
    if box:
        mask = mask.crop(box)
    mw, mh = mask.size

    # The wave is much wider than tall. Scale it to the tile's width and centre
    # it vertically, which keeps the shape readable when the whole icon is
    # 16 px across.
    canvas = 256
    pad = 0.14                              # margin inside the tile
    inner = canvas * (1 - 2 * pad)
    scale = min(inner / mw, inner / mh)
    fitted_size = (max(1, int(round(mw * scale))),
                   max(1, int(round(mh * scale))))
    fitted = mask.resize(fitted_size, Image.LANCZOS)

    # Compose: tile, then the wave drawn in white through the coverage mask.
    master = rounded_tile(canvas)
    layer = Image.new("RGBA", fitted_size, ART_RGB + (0,))
    layer.putalpha(fitted)

    pos = ((canvas - fitted_size[0]) // 2, (canvas - fitted_size[1]) // 2)
    master.alpha_composite(layer, dest=pos)

    frames = [master.resize((s, s), Image.LANCZOS) for s in SIZES]
    frames[0].save(args.out, format="ICO",
                   sizes=[(s, s) for s in SIZES], append_images=frames[1:])
    print(f"wrote {args.out}: {', '.join(str(s) for s in SIZES)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
