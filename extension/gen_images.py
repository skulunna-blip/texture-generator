"""Generate icon.png (128x128) and featured.png (1280x720) for the
TG Blender Link extension upload. 4x supersampling for crisp edges.

Theme: Blender-dark background, a shaded material ball with checker pattern,
and a chain-link (sync/bridge) motif.
"""
from PIL import Image, ImageDraw, ImageFont
import math, os

FONT_BOLD = "C:/Windows/Fonts/arialbd.ttf"
FONT_REG = "C:/Windows/Fonts/arial.ttf"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)))

BG_TOP = (52, 55, 60)
BG_BOT = (24, 25, 28)
GRID   = (255, 255, 255, 10)
BALL_DARK  = (34, 68, 82)
BALL_LIGHT = (96, 176, 198)
BALL_HI    = (220, 245, 252)
CHAIN = (245, 247, 250)


def gradient_bg(w, h):
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(h - 1, 1)
        color = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        d.line([(0, y), (w, y)], fill=color + (255,))
    return img


def draw_grid(d, w, h, step):
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=GRID)
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=GRID)


def ball_layer(w, h, cx, cy, r):
    """Material ball: checker base + radial shading + specular highlight."""
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    # checker base (drawn bigger than the circle, masked after)
    n = 10
    cell = r * 2 / n
    x0 = cx - r
    y0 = cy - r
    for i in range(n):
        for j in range(n):
            color = BALL_LIGHT if (i + j) % 2 == 0 else BALL_DARK
            d.rectangle([x0 + i * cell, y0 + j * cell,
                         x0 + (i + 1) * cell, y0 + (j + 1) * cell], fill=color + (255,))
    # circular mask
    mask = Image.new("L", (w, h), 0)
    dm = ImageDraw.Draw(mask)
    dm.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    # radial shading overlay (dark rim -> slightly lit center -> bright top-left)
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ds = ImageDraw.Draw(shade)
    for y in range(int(cy - r), int(cy + r)):
        for x in range(int(cx - r), int(cx + r)):
            dx, dy = x - cx, y - cy
            dist = math.hypot(dx, dy)
            if dist > r:
                continue
            edge = dist / r
            ang = math.atan2(dy, dx)
            # light from top-left
            lambert = max(0.0, (math.cos(ang) - math.sin(ang)) / math.sqrt(2.0))
            dark = 0.35 + 0.20 * edge
            lit = 0.60 * max(lambert, 0.0)
            v = min(1.0, dark + lit)
            v = 0.55 + 0.45 * v
            ds.point((x, y), fill=(0, 0, 0, int((1.0 - v) * 255)))
    layer = Image.composite(layer, Image.new("RGBA", (w, h), (0, 0, 0, 0)), mask)
    shade = Image.composite(shade, Image.new("RGBA", (w, h), (0, 0, 0, 0)), mask)
    out = Image.alpha_composite(layer, shade)
    # specular highlight blob top-left
    ds = ImageDraw.Draw(out)
    hr = r * 0.32
    hx, hy = cx - r * 0.45, cy - r * 0.5
    for yy in range(int(hy - hr), int(hy + hr)):
        for xx in range(int(hx - hr), int(hx + hr)):
            if math.hypot(xx - hx, yy - hy) <= hr:
                t = 1.0 - math.hypot(xx - hx, yy - hy) / hr
                a = int(120 * t)
                if a > 0:
                    ds.point((xx, yy), fill=BALL_HI + (a,))
    return out


def chain_layer(w, h, cx, cy, length, thickness, width, angle, color):
    """One chain link (stadium ring) rotated around its center."""
    size = int(length + thickness + width + 8)
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    half = length / 2
    bbox = [size / 2 - half - thickness / 2, size / 2 - thickness / 2,
            size / 2 + half + thickness / 2, size / 2 + thickness / 2]
    d.rounded_rectangle(bbox, radius=width, outline=color + (255,), width=width)
    img = img.rotate(angle, resample=Image.BICUBIC, center=(size / 2, size / 2))
    img = img.crop((0, 0, 0, 0))  # no-op; keeps reference
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(img, (int(cx - size / 2), int(cy - size / 2)), img)
    return out


def render_icon(path):
    SS = 4
    W = H = 128
    w = h = W * SS
    img = gradient_bg(w, h)
    d = ImageDraw.Draw(img)
    draw_grid(d, w, h, 24 * SS)
    cx = cy = w // 2
    r = int(46 * SS)
    ball = ball_layer(w, h, cx, cy, r)
    img = Image.alpha_composite(img, ball)
    # chain across the ball
    c1 = chain_layer(w, h, cx, cy, 34 * SS, 22 * SS, 8 * SS, -35, CHAIN)
    c2 = chain_layer(w, h, cx, cy, 34 * SS, 22 * SS, 8 * SS, 35, CHAIN)
    img = Image.alpha_composite(img, c1)
    img = Image.alpha_composite(img, c2)
    img = img.resize((W, H), Image.LANCZOS)
    img.convert("RGB").save(path)
    print("wrote", path)


def render_featured(path):
    SS = 3
    W, H = 1280, 720
    w, h = W * SS, H * SS
    img = gradient_bg(w, h)
    d = ImageDraw.Draw(img)
    draw_grid(d, w, h, 80 * SS)
    # glow behind the ball
    gx, gy, gr = int(320 * SS), int(370 * SS), int(200 * SS)
    for yy in range(gy - gr, gy + gr, max(1, SS // 2)):
        for xx in range(gx - gr, gx + gr, max(1, SS // 2)):
            dist = math.hypot(xx - gx, yy - gy)
            if dist <= gr:
                t = 1.0 - dist / gr
                a = int(28 * t * t)
                if a > 0:
                    d.point((xx, yy), fill=(110, 180, 205, a))
    # ball
    ball = ball_layer(w, h, gx, gy, int(175 * SS))
    img = Image.alpha_composite(img, ball)
    # chain motif near ball
    c1 = chain_layer(w, h, gx + int(150 * SS), gy - int(150 * SS),
                     120 * SS, 70 * SS, 22 * SS, -40, CHAIN)
    c2 = chain_layer(w, h, gx + int(210 * SS), gy - int(60 * SS),
                     120 * SS, 70 * SS, 22 * SS, 40, CHAIN)
    img = Image.alpha_composite(img, c1)
    img = Image.alpha_composite(img, c2)
    # text block on the right
    dc = ImageDraw.Draw(img)
    title = "TG Blender Link"
    sub = "Sync PBR textures into your current Blender scene"
    chip = "Blender 4.2+  •  Localhost bridge"
    tfont = ImageFont.truetype(FONT_BOLD, int(88 * SS))
    sfont = ImageFont.truetype(FONT_REG, int(30 * SS))
    cfont = ImageFont.truetype(FONT_REG, int(22 * SS))
    tx = int(640 * SS)
    ty = int(300 * SS)
    dc.text((tx, ty), title, font=tfont, fill=(242, 244, 247, 255))
    box = dc.textbbox((tx, ty), title, font=tfont)
    dc.text((tx, box[3] + int(28 * SS)), sub, font=sfont, fill=(176, 184, 192, 255))
    sbox = dc.textbbox((tx, box[3] + int(28 * SS)), sub, font=sfont)
    # chip pill behind chip text
    cbox = dc.textbbox((tx, sbox[3] + int(52 * SS)), chip, font=cfont)
    pad = int(14 * SS)
    dc.rounded_rectangle(
        [cbox[0] - pad, cbox[1] - int(6 * SS), cbox[2] + pad, cbox[3] + int(10 * SS)],
        radius=int(14 * SS), fill=(70, 140, 160, 90), outline=(110, 190, 210, 160),
        width=max(1, SS // 2))
    dc.text((tx, sbox[3] + int(52 * SS)), chip, font=cfont, fill=(206, 230, 238, 255))
    img = img.resize((W, H), Image.LANCZOS)
    img.convert("RGB").save(path)
    print("wrote", path)


render_icon(os.path.join(OUT, "icon.png"))
render_featured(os.path.join(OUT, "featured.png"))
