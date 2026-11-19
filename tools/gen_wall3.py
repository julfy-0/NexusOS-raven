from PIL import Image

w3 = Image.open("/mnt/c/Dev/NexusOS-P6P/assets/wallpapers/wall3.png")

# Target is 1440x3040
target_w, target_h = 1440, 3040
crop_w = int(1080 * target_w / target_h) # 511
left = (1920 - crop_w) // 2

# Crop portrait
crop = w3.crop((left, 0, left + crop_w, 1080))

# Resize to 360x760
thumb = crop.resize((360, 760), Image.LANCZOS)
thumb.save("/mnt/c/Dev/NexusOS-P6P/tools/wall3_360x760.png")

# Upscale back to 1440x3040 nearest
up4 = thumb.resize((target_w, target_h), Image.NEAREST)
up4.resize((360, 760)).save("/mnt/c/Dev/NexusOS-P6P/tools/wall3_preview_nearest.png")

print("Generated wall3_360x760.png and preview")
