#!/usr/bin/env python3
"""Makes a texture pack for ClassiCube (default.zip) from nothing: every picture drawn here, in its own
style (noise-shaded block colours, a font rendered from DejaVu Sans Mono), so the game is playable
without downloading the original one, and the pack can go anywhere.

    classicube-pack.py out/default.zip      (copy it to classicube.zip beside it too: the fallback pack)

ClassiCube only needs these files to exist (Resources.c) and reads them at fixed places: terrain.png
(16x16 tiles, the index of each block's faces in its Block.c table), default.png (a 16x16 grid of
characters), gui.png and icons.png (the hotbar, buttons, crosshair), and the mobs' skins."""
import io
import random
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFont

T = 16  # pixels per tile
rng = random.Random(1)


def noisy(base, spread=18, size=T, alpha=255):
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            d = rng.randint(-spread, spread)
            px[x, y] = tuple(max(0, min(255, c + d)) for c in base) + (alpha,)
    return img


def bordered(base, edge, spread=10):
    img = noisy(base, spread)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, T - 1, T - 1], outline=edge + (255,))
    return img


def bricks(base, mortar):
    img = noisy(base, 14)
    d = ImageDraw.Draw(img)
    for y in range(0, T, 4):
        d.line([0, y, T, y], fill=mortar + (255,))
        off = 0 if (y // 4) % 2 == 0 else 4
        for x in range(off, T, 8):
            d.line([x, y, x, y + 3], fill=mortar + (255,))
    return img


def ore(dots):
    img = noisy((125, 125, 125), 14)
    px = img.load()
    for _ in range(7):
        x, y = rng.randint(1, T - 3), rng.randint(1, T - 3)
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            px[x + dx, y + dy] = dots + (255,)
    return img


def sprite(draw_fn):
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(img))
    return img


def terrain():
    img = Image.new("RGBA", (16 * T, 16 * T), (0, 0, 0, 0))
    tiles = {}
    grass_top = noisy((96, 160, 64), 16)
    dirt = noisy((134, 96, 67), 16)
    side = dirt.copy()
    ImageDraw.Draw(side).rectangle([0, 0, T - 1, 3], fill=(96, 160, 64, 255))
    for x in range(0, T, 3):
        side.putpixel((x, 4), (96, 160, 64, 255))
    tiles.update({0: grass_top, 1: noisy((128, 128, 128), 16), 2: dirt, 3: side})
    planks = noisy((176, 140, 88), 10)
    for y in (3, 7, 11, 15):
        ImageDraw.Draw(planks).line([0, y, T, y], fill=(120, 90, 50, 255))
    tiles[4] = planks
    tiles[5] = bordered((168, 168, 168), (110, 110, 110))   # slab side
    tiles[6] = bordered((180, 180, 180), (120, 120, 120))   # slab top
    tiles[7] = bricks((150, 70, 55), (200, 190, 180))
    tnt = noisy((200, 40, 40), 10)
    ImageDraw.Draw(tnt).rectangle([0, 6, T - 1, 9], fill=(235, 235, 235, 255))
    tiles.update({8: tnt, 9: bordered((200, 60, 50), (90, 20, 20)), 10: bordered((190, 60, 50), (90, 20, 20))})
    tiles[11] = sprite(lambda d: d.line([8, 0, 8, 15], fill=(150, 110, 60, 255), width=2))  # rope
    tiles[12] = sprite(lambda d: (d.line([8, 15, 8, 7], fill=(40, 130, 40, 255)),
                                  d.ellipse([5, 2, 11, 8], fill=(210, 30, 40, 255))))          # rose
    tiles[13] = sprite(lambda d: (d.line([8, 15, 8, 7], fill=(40, 130, 40, 255)),
                                  d.ellipse([5, 2, 11, 8], fill=(240, 220, 40, 255))))         # dandelion
    tiles[14] = noisy((40, 80, 220), 12, alpha=170)                                         # water
    tiles[15] = sprite(lambda d: (d.line([8, 15, 8, 5], fill=(90, 60, 30, 255), width=2),
                                  d.ellipse([3, 1, 13, 9], fill=(50, 140, 50, 255))))          # sapling
    tiles[16] = bricks((120, 120, 120), (80, 80, 80))                                       # cobblestone
    tiles[17] = noisy((60, 60, 60), 30)                                                     # bedrock
    tiles[18] = noisy((220, 205, 150), 10)                                                  # sand
    tiles[19] = noisy((140, 130, 125), 30)                                                  # gravel
    log_side = noisy((105, 80, 50), 8)
    for x in (2, 6, 10, 13):
        ImageDraw.Draw(log_side).line([x, 0, x, T], fill=(80, 60, 35, 255))
    log_top = noisy((180, 150, 100), 8)
    ImageDraw.Draw(log_top).ellipse([3, 3, 12, 12], outline=(120, 90, 55, 255))
    tiles.update({20: log_side, 21: log_top})
    leaves = noisy((60, 140, 50), 25)
    for _ in range(40):
        leaves.putpixel((rng.randrange(T), rng.randrange(T)), (0, 0, 0, 0))
    tiles[22] = leaves
    for i, (top, mid, bot) in {23: ((220, 220, 225),) * 3, 24: ((250, 210, 60),) * 3}.items():
        tiles[i] = bordered(top, (150, 150, 150)); tiles[i + 16] = bordered(mid, (140, 140, 140)); tiles[i + 32] = bordered(bot, (130, 130, 130))
    tiles[25] = noisy((225, 210, 160), 8); tiles[41] = bricks((215, 200, 150), (180, 160, 110)); tiles[57] = noisy((205, 190, 140), 8)
    tiles[28] = sprite(lambda d: (d.rectangle([7, 9, 9, 15], fill=(230, 230, 220, 255)), d.ellipse([3, 3, 13, 10], fill=(200, 30, 30, 255))))
    tiles[29] = sprite(lambda d: (d.rectangle([7, 9, 9, 15], fill=(230, 230, 220, 255)), d.ellipse([3, 3, 13, 10], fill=(150, 110, 80, 255))))
    tiles[30] = noisy((240, 110, 20), 30)                                                   # lava
    tiles.update({32: ore((240, 200, 40)), 33: ore((220, 170, 140)), 34: ore((30, 30, 30))})
    shelf = noisy((176, 140, 88), 8)
    dr = ImageDraw.Draw(shelf)
    for y in (1, 9):
        for x in range(1, T - 1, 2):
            dr.rectangle([x, y, x, y + 5], fill=rng.choice([(150, 40, 40, 255), (40, 70, 150, 255), (60, 120, 50, 255)]))
    tiles[35] = shelf
    mossy = bricks((120, 120, 120), (80, 80, 80))
    for _ in range(30):
        mossy.putpixel((rng.randrange(T), rng.randrange(T)), (70, 130, 60, 255))
    tiles.update({36: mossy, 37: noisy((30, 20, 45), 10)})
    tiles[38] = sprite(lambda d: d.polygon([(8, 1), (13, 15), (3, 15)], fill=(250, 150, 30, 220)))  # fire
    tiles[48] = noisy((220, 220, 80), 30)                                                   # sponge
    glass = Image.new("RGBA", (T, T), (200, 230, 255, 40))
    ImageDraw.Draw(glass).rectangle([0, 0, T - 1, T - 1], outline=(230, 245, 255, 255))
    tiles.update({49: glass, 50: noisy((245, 248, 255), 5), 51: noisy((160, 200, 255), 8, alpha=190)})
    tiles[54] = bricks((230, 230, 225), (190, 190, 185))                                    # ceramic tile
    wool = [(220, 50, 50), (240, 140, 40), (240, 230, 60), (140, 220, 40), (50, 170, 50), (40, 170, 130),
            (60, 220, 220), (90, 180, 230), (60, 80, 220), (110, 60, 200), (150, 70, 220), (210, 70, 210),
            (240, 110, 170), (40, 40, 40), (130, 130, 130), (240, 240, 240)]
    for i, c in enumerate(wool):
        tiles[64 + i] = noisy(c, 10)
    for i, c in enumerate([(250, 180, 200), (40, 110, 40), (110, 75, 40), (30, 40, 140), (40, 200, 180)]):
        tiles[80 + i] = noisy(c, 10)
    for i, t in tiles.items():
        img.paste(t, ((i % 16) * T, (i // 16) * T))
    return img


def font():
    img = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 8)
    d = ImageDraw.Draw(img)
    for code in range(32, 127):
        x, y = (code % 16) * 8, (code // 16) * 8
        d.text((x, y - 1), chr(code), font=f, fill=(255, 255, 255, 255))
    px = img.load()
    for y in range(128):
        for x in range(128):
            px[x, y] = (255, 255, 255, 255) if px[x, y][3] > 110 else (0, 0, 0, 0)
    return img


def gui():
    img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 181, 21], fill=(40, 40, 50, 200), outline=(200, 200, 210, 255))   # hotbar
    for i in range(9):
        d.rectangle([1 + i * 20, 1, 20 + i * 20, 20], outline=(120, 120, 130, 255))
    d.rectangle([0, 22, 23, 45], outline=(255, 255, 255, 255), width=2)                   # selected slot
    for y, fill in ((46, (50, 50, 60)), (66, (90, 90, 110)), (86, (120, 130, 180))):      # buttons
        d.rectangle([0, y, 199, y + 19], fill=fill + (255,), outline=(20, 20, 25, 255))
    return img


def icons():
    img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.line([7, 0, 7, 14], fill=(255, 255, 255, 255))                                      # crosshair
    d.line([0, 7, 14, 7], fill=(255, 255, 255, 255))
    return img


def skin(color):
    img = noisy(color, 12, size=64).resize((64, 32))
    return img


def fire_frames():
    # 32 frames of 16x16 flame side by side: animations.txt's "6 2 0 0 16 32 0" (tile 38, fire)
    img = Image.new("RGBA", (16 * 32, 16), (0, 0, 0, 0))
    for f in range(32):
        frame = Image.new("RGBA", (T, T), (0, 0, 0, 0))
        d = ImageDraw.Draw(frame)
        sway = rng.randint(-2, 2)
        d.polygon([(8 + sway, 1 + f % 3), (13, 15), (3, 15)], fill=(250, 140 + rng.randint(0, 60), 30, 230))
        d.polygon([(8 + sway, 6), (11, 15), (5, 15)], fill=(255, 230, 120, 240))
        img.paste(frame, (f * T, 0))
    return img


ANIMATIONS_TXT = "# fire: tile x, tile y, frame x, frame y, frame size, frames, tick delay\r\n6 2 0 0 16 32 0"


def png(img):
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "default.zip"
    files = {
        "terrain.png": terrain(), "default.png": font(), "gui.png": gui(), "gui_classic.png": gui(),
        "icons.png": icons(), "particles.png": noisy((200, 200, 200), 20, size=128),
        "clouds.png": Image.new("RGBA", (256, 256), (255, 255, 255, 180)),
        "rain.png": Image.new("RGBA", (64, 64), (120, 150, 255, 140)), "snow.png": Image.new("RGBA", (64, 64), (255, 255, 255, 200)),
        "char.png": skin((200, 160, 120)), "creeper.png": skin((70, 170, 70)), "pig.png": skin((240, 170, 170)),
        "sheep.png": skin((230, 230, 230)), "sheep_fur.png": skin((245, 245, 245)), "skeleton.png": skin((210, 210, 200)),
        "spider.png": skin((50, 40, 40)), "zombie.png": skin((70, 120, 90)), "chicken.png": skin((250, 250, 245)),
        "animations.png": fire_frames(),
    }
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, img in files.items():
            z.writestr(name, png(img))
        z.writestr("animations.txt", ANIMATIONS_TXT)
    print(out)


if __name__ == "__main__":
    main()
