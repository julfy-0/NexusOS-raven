import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA").resize((64, 64), Image.LANCZOS)
open(sys.argv[2], "wb").write(im.tobytes())