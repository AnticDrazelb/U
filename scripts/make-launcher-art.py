# Crema's artwork, drawn here so its parts can move on their own in the launcher's intro:
#   launcher-assets/launcher/intro-console.png  the tilted console (no picture on its screen)
#   launcher-assets/launcher/intro-coffee.png   the coffee, steam and bar, on the same canvas and tilt
#   launcher-assets/launcher/intro-title.png    "Crema"
#   launcher-assets/launcher/logo.png           console and coffee together (menus' corner logo)
#   sce_sys/icon0.png                           the app icon (512x512, the PS4 home screen)
#   launcher-assets/xmb/sounds/intro.wav        the intro chime (synthesized, 48 kHz stereo)
# Needs Pillow (Windows python on this machine).
import math
import struct
import wave
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

root = Path(__file__).resolve().parent.parent
art = root / "launcher-assets" / "launcher"
art.mkdir(parents=True, exist_ok=True)
S = 2048  # drawn at 2x, saved at 1024
TILT = 8  # degrees, counter-clockwise (the left end lower)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def gradient(size, stops, horizontal=True):
    w, h = size
    img = Image.new("RGBA", size)
    px = img.load()
    n = w if horizontal else h
    for i in range(n):
        t = i / max(1, n - 1)
        for k in range(len(stops) - 1):
            (t0, c0), (t1, c1) = stops[k], stops[k + 1]
            if t0 <= t <= t1:
                color = lerp(c0, c1, (t - t0) / max(1e-6, t1 - t0))
                break
        for j in range(h if horizontal else w):
            if horizontal:
                px[i, j] = color + (255,)
            else:
                px[j, i] = color + (255,)
    return img


def rounded_mask(size, box, radius):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


cx, cy = S // 2, S // 2
body = (cx - 860, cy - 440, cx + 860, cy + 440)
screen = (cx - 470, cy - 300, cx + 470, cy + 300)


def console_layer():
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # glow behind the body
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle(body, radius=300, fill=(120, 110, 255, 170))
    layer = Image.alpha_composite(layer, glow.filter(ImageFilter.GaussianBlur(70)))
    # the body: cyan -> violet -> pink
    fill = gradient((S, S), [(0.0, (70, 200, 255)), (0.5, (120, 120, 245)), (1.0, (225, 80, 215))])
    shade = gradient((S, S), [(0.0, (255, 255, 255)), (1.0, (150, 150, 170))], horizontal=False)
    fill = ImageChops.multiply(fill, shade)
    layer.paste(fill, (0, 0), rounded_mask((S, S), body, 300))
    d = ImageDraw.Draw(layer)
    # the screen, with a faint diagonal sheen
    d.rounded_rectangle(screen, radius=60, fill=(30, 26, 58, 255))
    sheen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).polygon([(cx + 80, screen[1]), (cx + 330, screen[1]), (cx + 40, screen[3]), (cx - 210, screen[3])],
                                  fill=(255, 255, 255, 14))
    sheen_mask = rounded_mask((S, S), screen, 60)
    layer.paste(Image.alpha_composite(layer, sheen), (0, 0), sheen_mask)
    d = ImageDraw.Draw(layer)
    # left: a cyan button and a small bar; right: a white and a dark button, a pink bar
    d.ellipse((cx - 760, cy - 150, cx - 600, cy + 10), fill=(26, 32, 70, 255))
    d.ellipse((cx - 735, cy - 125, cx - 625, cy - 15), fill=(70, 230, 255, 255))
    d.rounded_rectangle((cx - 745, cy + 120, cx - 615, cy + 160), radius=20, fill=(40, 70, 160, 255))
    d.ellipse((cx + 610, cy - 250, cx + 720, cy - 140), fill=(255, 255, 255, 255))
    d.ellipse((cx + 540, cy - 110, cx + 640, cy - 10), fill=(26, 24, 52, 255))
    d.rounded_rectangle((cx + 600, cy + 90, cx + 730, cy + 130), radius=20, fill=(150, 50, 140, 255))
    return layer


def coffee_layer():
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    # steam: three wavy lines, violet / pink / cyan
    for i, color in enumerate([(170, 120, 255), (240, 110, 210), (110, 210, 255)]):
        x0 = cx - 90 + i * 90
        points = [(x0 + 22 * math.sin(k / 10 * math.pi * 2 + i), cy - 250 + k * 13) for k in range(11)]
        d.line(points, fill=color + (255,), width=26, joint="curve")
    # the cup: white to light blue, a violet rim band, the handle
    cup = (cx - 170, cy - 95, cx + 140, cy + 130)
    fill = gradient((S, S), [(0.0, (255, 255, 255)), (1.0, (175, 210, 255))], horizontal=False)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((cup[0], cup[1] - 60, cup[2], cup[3]), radius=110, fill=255)
    ImageDraw.Draw(mask).rectangle((cup[0], cup[1] - 60, cup[2], cup[1]), fill=0)
    layer.paste(fill, (0, 0), mask)
    d = ImageDraw.Draw(layer)
    d.ellipse((cup[2] - 30, cy - 70, cup[2] + 120, cy + 70), outline=(225, 235, 255, 255), width=32)
    d.rounded_rectangle((cup[0] + 30, cup[1] + 20, cup[2] - 30, cup[1] + 50), radius=15, fill=(170, 100, 255, 255))
    # the bar under it: cyan -> pink
    bar = gradient((S, S), [(0.0, (80, 220, 255)), (1.0, (240, 90, 210))])
    layer.paste(bar, (0, 0), rounded_mask((S, S), (cx - 220, cy + 185, cx + 220, cy + 215), 15))
    return layer


def tilt(img):
    return img.rotate(TILT, resample=Image.BICUBIC, center=(cx, cy))


def half(img):
    return img.resize((S // 2, S // 2), Image.LANCZOS)


console = tilt(console_layer())
coffee = tilt(coffee_layer())
half(console).save(art / "intro-console.png")
half(coffee).save(art / "intro-coffee.png")
both = Image.alpha_composite(console, coffee)
both.crop((cx - 960, cx - 960, cx + 960, cx + 960)).resize((256, 256), Image.LANCZOS).save(art / "logo.png")

# the title, in the launcher's bold font with the body's gradient
font = ImageFont.truetype(str(root / "launcher-assets" / "xmb" / "fonts" / "Nunito-ExtraBold.ttf"), 320)
title = Image.new("RGBA", (1600, 420), (0, 0, 0, 0))
mask = Image.new("L", title.size, 0)
md = ImageDraw.Draw(mask)
w = md.textlength("Crema", font=font)
md.text(((1600 - w) / 2, 10), "Crema", font=font, fill=255)
title.paste(gradient(title.size, [(0.0, (80, 220, 255)), (0.5, (160, 130, 255)), (1.0, (240, 100, 210))]), (0, 0), mask)
title.save(art / "intro-title.png")

# the app icon: night sky, waves at the bottom, the console with the coffee
icon = gradient((S, S), [(0.0, (30, 34, 66)), (1.0, (14, 14, 32))], horizontal=False)
d = ImageDraw.Draw(icon)
for i, (color, base) in enumerate([((40, 52, 86), 1560), ((52, 44, 88), 1700)]):
    points = [(x, base + 70 * math.sin(x / S * math.pi * 2 + i * 1.7)) for x in range(0, S + 1, 32)]
    d.polygon(points + [(S, S), (0, S)], fill=color + (255,))
icon = Image.alpha_composite(icon, both)
icon.convert("RGB").resize((512, 512), Image.LANCZOS).save(root / "sce_sys" / "icon0.png")

# the intro chime: two bell notes and a sparkle, each a few decaying harmonics
RATE = 48000
length = int(RATE * 1.8)
samples = [0.0] * length


def bell(start, freq, gain, decay):
    s0 = int(start * RATE)
    for i in range(s0, length):
        t = (i - s0) / RATE
        env = math.exp(-t * decay) * min(1.0, t * 400)
        v = sum(a * math.sin(2 * math.pi * freq * m * t) for m, a in ((1, 1.0), (2.01, 0.45), (3.02, 0.2), (4.2, 0.1)))
        samples[i] += gain * env * v


bell(0.00, 1318.5, 0.26, 3.2)  # E6
bell(0.12, 1975.5, 0.22, 3.6)  # B6
for k, f in enumerate((2637.0, 3136.0, 3951.1)):  # E7 G7 B7 sparkle
    bell(0.26 + k * 0.05, f, 0.07, 7.0)
peak = max(abs(v) for v in samples) or 1.0
with wave.open(str(root / "launcher-assets" / "xmb" / "sounds" / "intro.wav"), "wb") as out:
    out.setnchannels(2)
    out.setsampwidth(2)
    out.setframerate(RATE)
    frames = bytearray()
    for i, v in enumerate(samples):
        # a little stereo width: the sparkle leans right
        l = v / peak * 0.85
        r = l * (1.0 if i < RATE * 0.26 else 0.9)
        frames += struct.pack("<hh", int(l * 32767), int(r * 32767))
    out.writeframes(bytes(frames))
print("wrote", art, "and sce_sys/icon0.png and intro.wav")
