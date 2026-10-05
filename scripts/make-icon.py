# Builds sce_sys/icon0.png (512x512, the PS4 app icon) from Cemu's logo (dist/linux/info.cemu.Cemu.png,
# copied as sce_sys/cemu-logo.png). Needs Pillow (Windows python on this machine).
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

here = Path(__file__).resolve().parent.parent / "sce_sys"
icon = Image.new("RGB", (512, 512), (18, 22, 30))
logo = Image.open(here / "cemu-logo.png").convert("RGBA")
logo = logo.resize((330, 330), Image.LANCZOS)
icon.paste(logo, ((512 - logo.width) // 2, 50), logo)
draw = ImageDraw.Draw(icon)
try:
    font = ImageFont.truetype("arialbd.ttf", 64)
except OSError:
    font = ImageFont.load_default()
text = "Cemu PS4"
w = draw.textlength(text, font=font)
draw.text(((512 - w) / 2, 420), text, fill=(235, 240, 250), font=font)
icon.save(here / "icon0.png")
print("wrote", here / "icon0.png")
