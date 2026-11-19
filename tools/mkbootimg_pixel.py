#!/usr/bin/env python3
"""boot.img header v4 (Pixel 6/6 Pro): только ядро, без ramdisk. DTB и cmdline берутся
из штатного vendor_boot.  Использование: mkbootimg_pixel.py Image boot.img"""
import struct, sys

PAGE = 4096
def pad(b): return b + b"\0" * (-len(b) % PAGE)

kernel = open(sys.argv[1], "rb").read()
os_version = (12 << 25) | (0 << 18) | (0 << 11) | ((2022 - 2000) << 4) | 8   # 12.0.0, 2022-08
hdr = b"ANDROID!"
hdr += struct.pack("<IIII", len(kernel), 0, os_version, 4096)  # kernel, ramdisk, os_ver, header_size
hdr += struct.pack("<4I", 0, 0, 0, 0)                          # reserved
hdr += struct.pack("<I", 4)                                    # header_version
hdr += b"".ljust(1536, b"\0")                                  # cmdline
hdr += struct.pack("<I", 0)                                    # signature_size
open(sys.argv[2], "wb").write(pad(hdr) + pad(kernel))
print("boot.img: %d bytes, kernel %d bytes" % (len(pad(hdr)) + len(pad(kernel)), len(kernel)))
