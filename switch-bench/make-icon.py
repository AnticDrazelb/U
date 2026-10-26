# Builds sce_sys/icon0.png (512x512, the PS4 app icon): a processor chip with an "A64" die, the
# kind of code the benchmark recompiles. Drawn at 2x and scaled down for smooth edges. Needs Pillow
# and the DejaVu fonts.
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
c0, c1 = 262, 762  # the chip's square
for i in range(8):  # pins on all four sides
    p = c0 + 40 + i * 60
    for box in ((p, c0 - 70, p + 26, c0), (p, c1, p + 26, c1 + 70), (c0 - 70, p, c0, p + 26), (c1, p, c1 + 70, p + 26)):
        draw.rounded_rectangle(box, radius=6, fill=(196, 160, 82))
draw.rounded_rectangle((c0, c0, c1, c1), radius=48, fill=(44, 52, 66), outline=(120, 132, 152), width=10)
draw.rounded_rectangle((c0 + 70, c0 + 70, c1 - 70, c1 - 70), radius=28, fill=(230, 48, 48))
draw.text((S // 2, S // 2 - 6), "A64", font=ImageFont.truetype(bold, 150), fill=(255, 255, 255), anchor="mm")
draw.text((S // 2, 110), "CPU BENCH", font=ImageFont.truetype(bold, 92), fill=(235, 238, 245), anchor="mm")
draw.text((S // 2, 930), "dynarmic on PS4", font=ImageFont.truetype(bold, 64), fill=(150, 170, 200), anchor="mm")

out.parent.mkdir(exist_ok=True)
icon.resize((512, 512), Image.LANCZOS).save(out)
print(out)
