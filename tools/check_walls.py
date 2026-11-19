from PIL import Image
for i in range(1,6):
    img = Image.open(f'/mnt/c/Dev/NexusOS-P6P/assets/wallpapers/wall{i}.png')
    print(f'wall{i}.png: {img.width}x{img.height}')
