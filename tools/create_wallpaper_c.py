from PIL import Image

w3 = Image.open("/mnt/c/Dev/NexusOS-P6P/assets/wallpapers/wall3.png")

target_w, target_h = 1440, 3040
crop_w = int(1080 * target_w / target_h) # 511
left = (1920 - crop_w) // 2

# Crop portrait
crop = w3.crop((left, 0, left + crop_w, 1080))
thumb = crop.resize((360, 760), Image.LANCZOS).convert("RGB")

W, H = 360, 760
pixels = []
for y in range(H):
    for x in range(W):
        r, g, b = thumb.getpixel((x, y))
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        pixels.append(rgb565)

# Write header
with open("/mnt/c/Dev/NexusOS-P6P/kernel/drivers/gpu/gs101/wallpaper_wall3.h", "w") as f:
    f.write("""#ifndef NEXUS_WALLPAPER_WALL3_H
#define NEXUS_WALLPAPER_WALL3_H

#include <stdint.h>

#define WALLPAPER_W 360
#define WALLPAPER_H 760

extern const uint16_t g_wallpaper_wall3[WALLPAPER_W * WALLPAPER_H];

#endif
""")

# Write C file
with open("/mnt/c/Dev/NexusOS-P6P/kernel/drivers/gpu/gs101/wallpaper_wall3.c", "w") as f:
    f.write('#include "wallpaper_wall3.h"\n\n')
    f.write('const uint16_t g_wallpaper_wall3[WALLPAPER_W * WALLPAPER_H] = {\n')
    for i in range(0, len(pixels), 16):
        chunk = pixels[i:i+16]
        hex_chunk = [f"0x{px:04X}" for px in chunk]
        line = ", ".join(hex_chunk)
        if i + 16 < len(pixels):
            line += ","
        f.write("    " + line + "\n")
    f.write("};\n")

print("Created wallpaper_wall3.h and wallpaper_wall3.c successfully!")
