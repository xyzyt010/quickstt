from PIL import Image
import os

def png_to_ico(png_path, ico_path):
    img = Image.open(png_path).convert('RGBA')
    sizes = [(16,16), (32,32), (48,48), (64,64), (128,128), (256,256)]
    imgs = [img.resize(s, Image.LANCZOS) for s in sizes]
    imgs[-1].save(ico_path, format='ICO', append_images=imgs[:-1],
                  sizes=sizes)
    print(f"Created {ico_path}: {os.path.getsize(ico_path)} bytes")

png_to_ico("QuickSTT_App/app_icon.png", "QuickSTT_App/app.ico")
png_to_ico("QuickSTT_Server/server_icon.png", "QuickSTT_Server/server.ico")
print("Done - ICO files from actual SVGs")
