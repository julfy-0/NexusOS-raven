from PIL import Image

w3 = Image.open("/mnt/c/Dev/NexusOS-P6P/assets/wallpapers/wall3.png")
# Save a small thumbnail of wall3 to check
w3.resize((192, 108)).save("/mnt/c/Dev/NexusOS-P6P/tools/w3_thumb.png")
print("w3 thumbnail saved")
