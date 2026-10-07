# Checks a Mii parts resource made by Crema (CremaMii.cpp) the way FFL reads it, and renders Miis
# from it to a PNG (front and three-quarter views): the face's features drawn into the mask texture
# as FFLiRawMask.cpp does, the shapes placed as FFLiCharModelCreator.cpp does.
# Textures must be linear (the PC build of the generator); on the console they are tiled.
#   python check-crema-mii.py FFLResHigh.dat preview.png
import struct
import sys

import numpy as np
from PIL import Image

TEXTURE_PARTS = [3, 132, 62, 24, 12, 12, 9, 2, 37, 6, 18]
TEX = dict(beard=0, cap=1, eye=2, eyebrow=3, faceline=4, makeup=5, glass=6, mole=7, mouth=8, mustache=9, noseline=10)
SHAPE_PARTS = [4, 132, 132, 12, 1, 12, 18, 18, 132, 132, 132, 132]
SH = dict(beard=0, cap1=1, cap2=2, faceline=3, glass=4, mask=5, noseline=6, nose=7, hair1=8, hair2=9, forehead1=10, forehead2=11)

data = open(sys.argv[1], "rb").read()
problems = []


def check(cond, what):
    if not cond:
        problems.append(what)


magic, version, uncompress, marker, expand = struct.unpack_from(">4sIII I", data, 0)
check(magic == b"FFRA" and version == 0x70000 and expand == 0, "header")


def tables(at, counts):
    maxima = struct.unpack_from(">%dI" % len(counts), data, at)
    at += 4 * len(counts)
    infos = []
    for n in counts:
        infos.append([struct.unpack_from(">3I4B", data, at + 16 * i) for i in range(n)])
        at += 16 * n
    return maxima, infos, at


tex_max, tex_info, at = tables(0x14, TEXTURE_PARTS)
shape_max, shape_info, at = tables(at, SHAPE_PARTS)
check(at == 0x49D0, "header size")


def load_texture(t, i):
    pos, size, csize, *_ , strategy = tex_info[t][i]
    check(strategy == 5 and size == csize and pos % 0x800 == 0 and pos + size <= len(data) and size <= tex_max[t], "texture %d/%d info" % (t, i))
    raw = data[pos:pos + size]
    mip_offset, w, h, mips, fmt = struct.unpack_from(">IHHBB", raw, size - 12)
    ch = [1, 2, 4][fmt]
    check(mips == 1 and len(raw) - 12 >= w * h * ch, "texture %d/%d footer" % (t, i))
    return np.frombuffer(raw[:w * h * ch], np.uint8).reshape(h, w, ch).astype(np.float32) / 255


def load_shape(s, i):
    pos, size, csize, *_, strategy = shape_info[s][i]
    check(strategy == 5 and size == csize and pos % 0x40 == 0 and pos + size <= len(data) and size <= shape_max[s], "shape %d/%d info" % (s, i))
    raw = data[pos:pos + size]
    epos = struct.unpack_from(">6I", raw, 0)
    esize = struct.unpack_from(">6I", raw, 24)
    bbox = struct.unpack_from(">6f", raw, 48)
    transform = struct.unpack_from(">18f", raw, 72)
    n = esize[0] // 16
    check(n > 0 and esize[1] == 4 * n and esize[3] == 4 * n and esize[4] in (4, 4 * n) and esize[2] in (0, 8 * n), "shape %d/%d sizes" % (s, i))
    for e in range(5):
        check(esize[e] == 0 or epos[e] % 0x40 == 0, "shape %d/%d alignment" % (s, i))
    check(epos[5] % 0x20 == 0 and esize[5] % 3 == 0 and epos[5] + 2 * esize[5] <= len(raw), "shape %d/%d indices" % (s, i))
    position = np.array(struct.unpack_from(">%df" % (4 * n), raw, epos[0]), np.float32).reshape(n, 4)[:, :3]
    packed = np.array(struct.unpack_from(">%dI" % n, raw, epos[1]), np.uint32)
    normal = np.stack([((packed >> s_) & 0x3FF).astype(np.int32) for s_ in (0, 10, 20)], 1)
    normal = np.where(normal >= 512, normal - 1024, normal) / 511.0
    uv = np.array(struct.unpack_from(">%df" % (2 * n), raw, epos[2]), np.float32).reshape(n, 2) if esize[2] else None
    index = np.array(struct.unpack_from(">%dH" % esize[5], raw, epos[5]), np.int32).reshape(-1, 3)
    check(index.max() < n, "shape %d/%d index range" % (s, i))
    check(np.allclose(bbox[:3], position.min(0), atol=1e-3) and np.allclose(bbox[3:], position.max(0), atol=1e-3), "shape %d/%d bounding box" % (s, i))
    return dict(position=position, normal=normal, uv=uv, index=index, transform=transform)


# every part loads
for t, n in enumerate(TEXTURE_PARTS):
    for i in range(n):
        load_texture(t, i)
for s, n in enumerate(SHAPE_PARTS):
    for i in range(n):
        load_shape(s, i)

# ---- a Mii (default parts values of the Mii editor), colors from FFLiColor.cpp
SKIN = np.array([1.0, 0.827, 0.678])
HAIR = np.array([0.25, 0.125, 0.06])
EYE_COLOR = np.array([0.275, 0.329, 0.659])
LIP, LIP_DARK = np.array([0.847, 0.322, 0.031]), np.array([0.51, 0.188, 0.094])
CYAN = np.array([0.0, 1.0, 1.0])


def sample(tex, u, v):
    h, w, _ = tex.shape
    x = np.clip((u * w).astype(int), 0, w - 1)
    y = np.clip((v * h).astype(int), 0, h - 1)
    return tex[y, x]


def mask_texture(eye, eyebrow, mouth, res=256):
    # FFLiRawMask.cpp: 64 unit space, y down; the quads' extra 0.8896 x 0.9277 scale
    out = np.zeros((res, res, 4), np.float32)
    k = res / 64
    ys, xs = np.mgrid[0:res, 0:res]
    px, py = (xs + 0.5) / k, (ys + 0.5) / k

    def put(tex, cx, cy, sx, sy, origin, colorize):
        sx, sy = sx * 0.88961464, sy * 0.9276675
        if origin == "left":
            x0, x1, u_at_x0 = cx - sx, cx, 0.0
        elif origin == "right":
            x0, x1, u_at_x0 = cx, cx + sx, 1.0
        else:
            x0, x1, u_at_x0 = cx - sx / 2, cx + sx / 2, 0.0
        inside = (px >= x0) & (px < x1) & (py >= cy - sy / 2) & (py < cy + sy / 2)
        u = (px - x0) / sx
        u = u if u_at_x0 == 0 else 1 - u
        v = (py - (cy - sy / 2)) / sy
        texel = sample(tex, u[inside], v[inside])
        rgb, a = colorize(texel)
        dst = out[inside]
        dst[:, :3] = dst[:, :3] * (1 - a[:, None]) + rgb * a[:, None]
        dst[:, 3] = np.maximum(dst[:, 3], a)
        out[inside] = dst

    mode2 = lambda r, g, b: (lambda t: (t[:, 0:1] * r + t[:, 1:2] * g + t[:, 2:3] * b, t[:, 3]))
    mode3 = lambda c: (lambda t: (t[:, 0:1] * c, t[:, 0]))
    spacing = 2 * 0.88961464 + 3.5323312
    eye_y = 12 * 1.0760943 + 4.629278 + 13.822246
    brow_y = 10 * 1.0760943 + 4.629278 + 11.920528
    mouth_y = 13 * 1.0760943 + 4.629278 + 24.629572
    eye_tex, brow_tex, mouth_tex = load_texture(TEX["eye"], eye), load_texture(TEX["eyebrow"], eyebrow), load_texture(TEX["mouth"], mouth)
    s = 0.4 * 4 + 1
    for cx, origin in ((32 - spacing, "left"), (32 + spacing, "right")):
        put(eye_tex, cx, eye_y, 5.34375 * s, 4.5 * s, origin, mode2(CYAN, np.ones(3), EYE_COLOR))
        put(brow_tex, cx, brow_y, 5.0625 * s, 4.5 * s, origin, mode3(HAIR))
    put(mouth_tex, 32, mouth_y, 6.1875 * s, 4.5 * s, "center", mode2(LIP, LIP_DARK, np.ones(3)))
    return out


def render(parts, yaw, size=360):
    color = np.ones((size, size, 3), np.float32) * 0.92
    depth = np.full((size, size), -1e9, np.float32)
    c, s = np.cos(yaw), np.sin(yaw)
    light = np.array([0.3, 0.5, 0.8]) / np.linalg.norm([0.3, 0.5, 0.8])
    for part in parts:
        p = part["position"] * part.get("scale", 1) + part["offset"]
        n = part["normal"]
        # rotate about y (the camera orbits), project orthographically: 70 units across
        x = p[:, 0] * c + p[:, 2] * s
        z = -p[:, 0] * s + p[:, 2] * c
        nx = n[:, 0] * c + n[:, 2] * s
        nz = -n[:, 0] * s + n[:, 2] * c
        shade = np.clip(np.stack([nx, n[:, 1], nz], 1) @ light, 0, 1) * 0.6 + 0.4
        sx = (x / 70 + 0.5) * size
        sy = (0.5 - (p[:, 1] - 30) / 70) * size
        for tri in part["index"]:
            a, b, d = tri
            # back faces away (the faces' winding is counter-clockwise from the front)
            area = (sx[b] - sx[a]) * (sy[d] - sy[a]) - (sx[d] - sx[a]) * (sy[b] - sy[a])
            if area >= 0:
                continue
            x0, x1 = int(max(0, min(sx[a], sx[b], sx[d]))), int(min(size - 1, max(sx[a], sx[b], sx[d])) + 1)
            y0, y1 = int(max(0, min(sy[a], sy[b], sy[d]))), int(min(size - 1, max(sy[a], sy[b], sy[d])) + 1)
            if x1 <= x0 or y1 <= y0:
                continue
            gy, gx = np.mgrid[y0:y1, x0:x1] + 0.5
            w0 = ((sx[b] - gx) * (sy[d] - gy) - (sx[d] - gx) * (sy[b] - gy)) / area
            w1 = ((sx[d] - gx) * (sy[a] - gy) - (sx[a] - gx) * (sy[d] - gy)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            zz = w0 * z[a] + w1 * z[b] + w2 * z[d]
            region = depth[y0:y1, x0:x1]
            visible = inside & (zz > region - part.get("bias", 0))
            if not visible.any():
                continue
            sh = (w0 * shade[a] + w1 * shade[b] + w2 * shade[d])[visible]
            if part.get("texture") is not None:
                uv = part["uv"]
                u = (w0 * uv[a, 0] + w1 * uv[b, 0] + w2 * uv[d, 0])[visible]
                v = (w0 * uv[a, 1] + w1 * uv[b, 1] + w2 * uv[d, 1])[visible]
                texel = sample(part["texture"], u, v)
                rgb, alpha = texel[:, :3], texel[:, 3]
            else:
                rgb, alpha = np.tile(part["color"], (visible.sum(), 1)), np.ones(visible.sum())
            target = color[y0:y1, x0:x1]
            target[visible] = target[visible] * (1 - alpha[:, None]) + rgb * sh[:, None] * alpha[:, None]
            color[y0:y1, x0:x1] = target
            if part.get("bias", 0) == 0:
                region[visible] = zz[visible]
                depth[y0:y1, x0:x1] = region
    return (np.clip(color, 0, 1) * 255).astype(np.uint8)


def mii(face, hair, eye, eyebrow, mouth, nose):
    faceline = load_shape(SH["faceline"], face)
    hair_pos = np.array(faceline["transform"][0:3])
    face_center = np.array(faceline["transform"][3:6])
    nose_scale = 4 * 0.175 + 0.4
    nose_pos = face_center + np.array([0, (9 - 8) * -1.5, 0])
    mask = mask_texture(eye, eyebrow, mouth)
    zero = np.zeros(3)
    return [
        dict(load_shape(SH["faceline"], face), offset=zero, color=SKIN),
        dict(load_shape(SH["forehead1"], hair), offset=hair_pos, color=SKIN),
        dict(load_shape(SH["hair1"], hair), offset=hair_pos, color=HAIR),
        dict(load_shape(SH["nose"], nose), offset=nose_pos, scale=nose_scale, color=SKIN),
        dict(load_shape(SH["mask"], face), offset=zero, texture=mask, bias=0.5),
    ]


rows = []
for row in range(2):
    views = []
    for col in range(4):
        n = row * 4 + col
        parts = mii(face=n % 12, hair=[0, 1, 2, 3, 4, 5, 6, 7][n] * 1, eye=n * 3, eyebrow=n * 2, mouth=n, nose=n)
        # hair types whose style (CremaMii.cpp StyleOf) is n
        style_of = lambda h: (h * 5 + h // 7) % 8
        parts[1] = dict(load_shape(SH["forehead1"], next(h for h in range(132) if style_of(h) == n)), offset=parts[1]["offset"], color=SKIN)
        parts[2] = dict(load_shape(SH["hair1"], next(h for h in range(132) if style_of(h) == n)), offset=parts[2]["offset"], color=HAIR)
        views.append(np.concatenate([render(parts, 0), render(parts, 0.7)], 0))
    rows.append(np.concatenate(views, 1))
Image.fromarray(np.concatenate(rows, 0)).save(sys.argv[2])
print("%d bytes; %s" % (len(data), "OK" if not problems else "problems: " + ", ".join(sorted(set(problems))[:20])))
