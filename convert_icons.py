import os
import sys
from PIL import Image
from PyQt6.QtWidgets import QApplication
from PyQt6.QtSvg import QSvgRenderer
from PyQt6.QtGui import QPainter, QImage
from PyQt6.QtCore import Qt, QRectF, QSizeF

def svg_to_ico(svg_path, ico_path):
    print(f"Converting {svg_path} to {ico_path}...")
    app = QApplication.instance() or QApplication(sys.argv)
    
    try:
        renderer = QSvgRenderer(svg_path)
        if not renderer.isValid():
            print(f"Invalid SVG: {svg_path}")
            return

        base_size = 512
        # Use QImage directly
        img = QImage(base_size, base_size, QImage.Format.Format_ARGB32_Premultiplied)
        img.fill(Qt.GlobalColor.transparent)

        painter = QPainter(img)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)

        svg_size = QSizeF(renderer.defaultSize())
        if svg_size.isEmpty():
            svg_size = QSizeF(256, 256)

        scaled_size = svg_size.scaled(base_size, base_size, Qt.AspectRatioMode.KeepAspectRatio)
        x = (base_size - scaled_size.width()) / 2
        y = (base_size - scaled_size.height()) / 2
        
        target_rect = QRectF(x, y, scaled_size.width(), scaled_size.height())
        renderer.render(painter, target_rect)
        painter.end()

        temp_png = "temp_icon.png"
        img.save(temp_png, "PNG")
        
        pil_img = Image.open(temp_png)
        # Multi-size ICO
        pil_img.save(ico_path, format='ICO', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
        
        if os.path.exists(temp_png):
            os.remove(temp_png)
        print(f"Successfully saved {ico_path}")
    except Exception as e:
        print(f"Failed to convert icon: {e}")

if __name__ == "__main__":
    app_root = r"C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app"
    svg_app = os.path.join(app_root, "QuickSTT_App", "Untitled-1.svg")
    svg_server = os.path.join(app_root, "QuickSTT_App", "the-server-4.svg")
    
    svg_to_ico(svg_app, os.path.join(app_root, "icon_app.ico"))
    svg_to_ico(svg_server, os.path.join(app_root, "icon_server.ico"))
