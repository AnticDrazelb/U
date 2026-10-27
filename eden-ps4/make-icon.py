# Builds sce_sys/icon0.png (512x512, the PS4 app icon): a Switch's two Joy-Con around a terminal,
# the text-only Switch program this stage runs. Drawn at 2x and scaled down for smooth edges.
# Needs Pillow and the DejaVu fonts.
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

out = Path(__file__).resolve().parent / "sce_sys" / "icon0.png"
S = 1024
icon = Image.new("RGB", (S, S))
draw = ImageDraw.Draw(icon)


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


for y in range(S):
    draw.line((0, y, S, y), fill=mix((28, 36, 52), (10, 13, 20), y / S))

bold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
mono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
top, bottom = 300, 720
draw.rounded_rectangle((132, top, 262, bottom), radius=60, fill=(0, 174, 219))   # left Joy-Con
draw.rounded_rectangle((762, top, 892, bottom), radius=60, fill=(255, 60, 40))   # right Joy-Con
draw.ellipse((172, 400, 222, 450), fill=(30, 34, 40))                            # stick
draw.ellipse((802, 560, 852, 610), fill=(30, 34, 40))
draw.rounded_rectangle((262, top, 762, bottom), radius=10, fill=(40, 44, 52))    # the screen's frame
draw.rectangle((292, top + 30, 732, bottom - 30), fill=(12, 16, 22))             # a text console
lines = ["> hello", "  checks", "  passed", "> _"]
for i, line in enumerate(lines):
    draw.text((320, top + 60 + i * 80), line, font=ImageFont.truetype(mono, 62), fill=(126, 231, 135))
draw.text((S // 2, 150), "EDEN", font=ImageFont.truetype(bold, 140), fill=(235, 238, 245), anchor="mm")
draw.text((S // 2, 880), "Switch core on PS4", font=ImageFont.truetype(bold, 64), fill=(150, 170, 200), anchor="mm")
icon.resize((512, 512), Image.LANCZOS).save(out)
print(out)
