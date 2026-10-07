# Builds sce_sys/icon0.png (512x512, the PS4 app icon): Crema, a cup of espresso seen from above
# with its crema and a latte-art heart. Drawn at 2x and scaled down for smooth edges.
# Needs Pillow (Windows python on this machine).
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

here = Path(__file__).resolve().parent.parent / "sce_sys"
S = 1024
icon = Image.new("RGB", (S, S))
draw = ImageDraw.Draw(icon)


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def circle(cx, cy, r, fill):
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)


# background: dark roast, lighter at the top
for y in range(S):
    draw.line((0, y, S, y), fill=mix((52, 32, 22), (18, 11, 8), y / S))

cx, cy = S // 2, 400
# saucer with a soft shadow, then the cup and its handle
circle(cx + 10, cy + 18, 336, (12, 7, 5))
circle(cx, cy, 330, (226, 216, 200))
circle(cx, cy, 300, (238, 230, 216))
draw.rounded_rectangle((cx + 230, cy - 46, cx + 345, cy + 46), radius=46, fill=(250, 246, 240))
circle(cx, cy, 262, (250, 246, 240))
# the crema: golden at the middle, darker towards the rim
for r in range(226, 0, -2):
    circle(cx, cy, r, mix((196, 138, 76), (104, 58, 28), (r / 226) ** 1.6))
# latte-art heart
heart = (247, 229, 196)
circle(cx - 58, cy - 22, 76, heart)
circle(cx + 58, cy - 22, 76, heart)
draw.polygon([(cx - 131, cy + 4), (cx + 131, cy + 4), (cx, cy + 150)], fill=heart)

try:
    font = ImageFont.truetype("georgiab.ttf", 168)
except OSError:
    font = ImageFont.load_default()
text = "Crema"
w = draw.textlength(text, font=font)
draw.text(((S - w) / 2, 780), text, fill=(245, 228, 200), font=font)

icon.resize((512, 512), Image.LANCZOS).save(here / "icon0.png")
print("wrote", here / "icon0.png")
