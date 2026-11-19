from PIL import Image

up = Image.open("/mnt/c/Users/sdc99/.gemini/antigravity/brain/6d3a6667-c420-4942-8837-3401e1e06af9/.user_uploaded/media_1795026384217_a6398240.png")
w3 = Image.open("/mnt/c/Dev/NexusOS-P6P/assets/wallpapers/wall3.png")

# up is 472x1024, aspect ratio is 1024 / 472 = 2.169
# Pixel 6 Pro is 1440x3040, aspect ratio is 3040 / 1440 = 2.111
# In w3 (1920x1080), let's crop the center flower swirl.
# If height is 1080, width for 1440x3040 is 1080 / 2.111 = 511.6
# Center is x = 1920 / 2 = 960.
# Crop from x = (960 - 256) to (960 + 256) = 704 to 1216.
crop_w = int(1080 * 1440 / 3040)  # ~511
left = (1920 - crop_w) // 2
crop = w3.crop((left, 0, left + crop_w, 1080))
crop.resize((up.width, up.height)).save("/mnt/c/Dev/NexusOS-P6P/tools/w3_crop_test.png")
print("Saved w3_crop_test.png, crop box:", left, 0, left + crop_w, 1080)
