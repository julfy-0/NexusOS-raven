#!/usr/bin/env python3
"""Convert a PNG into the NexusOS mouse-cursor blob (assets/cursors/cursor.bin).

    python3 tools/make_cursor.py assets/cursors/cursor.png assets/cursors/cursor.bin

The hotspot (the pixel that is the "tip" of the pointer, in the PNG's own
coordinates) is read from assets/cursors/hotspot.txt next to the PNG
("x y", default "0 0"), or given with --hotspot X Y.

Blob layout, little endian:
    4 bytes  magic  "NXCR"
    u16 width, u16 height, u16 hotspot_x, u16 hotspot_y
    width*height*4 bytes  RGBA, straight (non-premultiplied) alpha

Requires Pillow (pip install pillow). Images larger than 64x64 are scaled
down to fit. Use a PNG with a transparent background.
"""
import os, struct, sys

MAX = 64

def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__); return 2
    src, dst = argv[1], argv[2]
    hot = None
    if "--hotspot" in argv:
        i = argv.index("--hotspot"); hot = (int(argv[i + 1]), int(argv[i + 2]))
    if hot is None:
        hp = os.path.join(os.path.dirname(os.path.abspath(src)), "hotspot.txt")
        hot = (0, 0)
        if os.path.exists(hp):
            parts = open(hp).read().split()
            if len(parts) >= 2: hot = (int(parts[0]), int(parts[1]))
    try:
        from PIL import Image
    except ImportError:
        sys.stderr.write("make_cursor.py: Pillow is missing (pip install pillow); keeping existing cursor.bin\n")
        return 1
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    if max(w, h) > MAX:
        k = MAX / float(max(w, h))
        nw, nh = max(1, round(w * k)), max(1, round(h * k))
        im = im.resize((nw, nh), Image.LANCZOS)
        hot = (round(hot[0] * k), round(hot[1] * k)); w, h = nw, nh
    hx = min(max(hot[0], 0), w - 1); hy = min(max(hot[1], 0), h - 1)
    with open(dst, "wb") as f:
        f.write(b"NXCR" + struct.pack("<HHHH", w, h, hx, hy) + im.tobytes())
    print("cursor: %dx%d, hotspot (%d,%d) -> %s" % (w, h, hx, hy, dst))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
