# Mouse cursor

Put your own picture here as `cursor.png` and rebuild (`make -j2 iso` / `./build_pc.sh`).

* PNG with a **transparent background**, RGBA. Best size 24-32 px (max 64x64, bigger images are scaled down).
* `hotspot.txt` - two numbers `x y`: the pixel of the picture that is the pointer's tip
  (an arrow: `1 1`; a crosshair: its centre). Coordinates are in the PNG's own pixels.
* The build converts it with `tools/make_cursor.py` (needs `pip install pillow`) into `cursor.bin`
  and embeds it in the kernel. If Pillow is missing the previous `cursor.bin` is kept.
* If `cursor.bin` is damaged the kernel falls back to a built-in arrow.
