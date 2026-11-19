#!/usr/bin/env python3
"""Convert assets/wallpapers/wall<N>.png into the NexusOS wallpaper blob.

    python3 tools/make_wallpaper.py <N> assets/wallpapers/wall<N>.nxw

If wall<N>.png is missing (or Pillow is not installed) a small gradient
placeholder is written instead, so the build never breaks.

Blob layout, little endian:
    4 bytes  magic "NXWP"
    u16 width, u16 height
    width*height*2 bytes  RGB565
The image is scaled to cover WIDTH x HEIGHT (centre crop, 16:9).
"""
import os, struct, sys

W, H = 800, 450

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def placeholder(n):
    pal = [((40, 60, 120), (150, 90, 200)), ((20, 90, 90), (90, 200, 140)),
           ((120, 50, 50), (240, 160, 80)), ((30, 30, 60), (110, 110, 190)),
           ((60, 60, 60), (170, 170, 170))]
    a, b = pal[(n - 1) % len(pal)]
    w, h = 32, 18
    out = bytearray()
    for y in range(h):
        for x in range(w):
            t = (x / (w - 1) + y / (h - 1)) / 2
            out += struct.pack("<H", rgb565(*[int(a[i] + (b[i] - a[i]) * t) for i in range(3)]))
    return w, h, bytes(out)

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    n, dst = int(argv[1]), argv[2]
    src = os.path.join(os.path.dirname(os.path.abspath(dst)), "wall%d.png" % n)
    data = None
    if os.path.exists(src):
        try:
            from PIL import Image
            im = Image.open(src).convert("RGB")
            k = max(W / im.width, H / im.height)
            nw, nh = max(W, round(im.width * k)), max(H, round(im.height * k))
            im = im.resize((nw, nh), Image.LANCZOS)
            l, t = (nw - W) // 2, (nh - H) // 2
            im = im.crop((l, t, l + W, t + H))
            raw = im.tobytes()
            out = bytearray()
            for i in range(0, len(raw), 3):
                out += struct.pack("<H", rgb565(raw[i], raw[i + 1], raw[i + 2]))
            data = (W, H, bytes(out))
            print("wallpaper %d: %s -> %dx%d" % (n, src, W, H))
        except ImportError:
            sys.stderr.write("make_wallpaper.py: Pillow is missing (pip install pillow); using placeholder\n")
    if data is None:
        data = placeholder(n)
    w, h, px = data
    with open(dst, "wb") as f:
        f.write(b"NXWP" + struct.pack("<HH", w, h) + px)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
