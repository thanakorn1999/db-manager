# Draws the app icon: resources/icon.png (1024), icon.icns (macOS), icon.ico (Windows), and
# social-preview.png (1280x640, GitHub Settings -> Social preview; uses macOS's Helvetica Neue).
# Run from the repo root: python3 resources/make_icon.py  (needs Pillow + numpy; iconutil on macOS)
import os, shutil, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4  # supersampling; drawn at 4096, scaled down for smooth edges
N = 1024 * S
HERE = os.path.dirname(os.path.abspath(__file__))


def px(v):
    return int(round(v * S))


def squircle(x0, y0, size, n=5.0, steps=720):
    # macOS-style continuous corner: |x|^n + |y|^n = 1
    r = size / 2
    cx, cy = x0 + r, y0 + r
    t = np.linspace(0, 2 * np.pi, steps, endpoint=False)
    c, s = np.cos(t), np.sin(t)
    x = cx + r * np.sign(c) * np.abs(c) ** (2 / n)
    y = cy + r * np.sign(s) * np.abs(s) ** (2 / n)
    return [(px(a), px(b)) for a, b in zip(x, y)]


def gradient(size, top, bottom, angle_mix=0.35):
    # diagonal gradient top-left -> bottom-right
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    t = (yy / h) * (1 - angle_mix) + (xx / w) * angle_mix
    t = t[..., None]
    a, b = np.array(top, np.float32), np.array(bottom, np.float32)
    rgb = a + (b - a) * t
    return Image.fromarray(rgb.clip(0, 255).astype(np.uint8), "RGB")


def cylinder_shade(w, h, light, dark):
    # horizontal shading across a cylinder body: bright a bit left of centre, darker at the edges
    x = np.linspace(-1, 1, w, dtype=np.float32)
    t = np.clip(np.abs(x + 0.25) / 1.25, 0, 1) ** 1.6
    row = np.array(light, np.float32) + (np.array(dark, np.float32) - np.array(light, np.float32)) * t[:, None]
    return Image.fromarray(np.repeat(row[None], h, 0).clip(0, 255).astype(np.uint8), "RGB")


def main():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))

    # tile shadow (macOS grid: 824 body at 100, soft drop shadow)
    shadow = Image.new("L", (N, N), 0)
    ImageDraw.Draw(shadow).polygon(squircle(100, 112, 824), fill=110)
    shadow = shadow.filter(ImageFilter.GaussianBlur(px(18)))
    img.paste((10, 12, 40, 255), (0, 0), shadow)

    # tile: indigo -> teal
    tile_mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(tile_mask).polygon(squircle(100, 100, 824), fill=255)
    img.paste(gradient((N, N), (79, 70, 229), (8, 145, 178)), (0, 0), tile_mask)

    # soft top glow on the tile
    glow = Image.new("L", (N, N), 0)
    ImageDraw.Draw(glow).ellipse([px(-100), px(-500), px(1124), px(420)], fill=45)
    glow = glow.filter(ImageFilter.GaussianBlur(px(110)))
    glow = Image.fromarray(np.minimum(np.asarray(glow), np.asarray(tile_mask)))
    img.paste((255, 255, 255, 255), (0, 0), glow)

    # database: three stacked discs
    x0, x1 = 302, 722
    ry = 62  # ellipse half-height
    h, gap, top = 132, 26, 318
    tops = [top + i * (h + gap) for i in range(3)]

    # shadow under the stack
    sh = Image.new("L", (N, N), 0)
    ImageDraw.Draw(sh).ellipse([px(x0 + 10), px(tops[-1] + h - ry + 30), px(x1 - 10), px(tops[-1] + h + ry + 40)], fill=120)
    sh = sh.filter(ImageFilter.GaussianBlur(px(22)))
    img.paste((15, 20, 60, 255), (0, 0), sh)

    body_fill = cylinder_shade(px(x1 - x0), N, (255, 255, 255), (199, 210, 254))
    for yt in reversed(tops):  # bottom first: each disc covers the top face of the one below
        yb = yt + h
        m = Image.new("L", (N, N), 0)
        d = ImageDraw.Draw(m)
        d.rectangle([px(x0), px(yt), px(x1), px(yb)], fill=255)
        d.ellipse([px(x0), px(yb - ry), px(x1), px(yb + ry)], fill=255)
        layer = Image.new("RGB", (N, N))
        layer.paste(body_fill, (px(x0), 0))
        img.paste(layer, (0, 0), m)
        # top face: the lid is light, lower ones only show as a darker groove
        face = (238, 242, 255, 255) if yt == tops[0] else (165, 180, 252, 255)
        ImageDraw.Draw(img).ellipse([px(x0), px(yt - ry), px(x1), px(yt + ry)], fill=face)

    # top face detail: inner ring, like a lid
    d = ImageDraw.Draw(img)
    yt = tops[0]
    inset = 46
    d.ellipse([px(x0 + inset), px(yt - ry + inset * ry / 210), px(x1 - inset), px(yt + ry - inset * ry / 210)],
              outline=(165, 180, 252, 255), width=px(7))

    # status dots on each disc, like drive LEDs
    for i, yt in enumerate(tops):
        cy = yt + h / 2 + ry * 0.55
        cx = x1 - 70
        col = (16, 185, 129) if i == 0 else (99, 102, 241)
        r = 15
        d.ellipse([px(cx - r), px(cy - r), px(cx + r), px(cy + r)], fill=col + (255,))

    out = img.resize((1024, 1024), Image.LANCZOS)
    png = os.path.join(HERE, "icon.png")
    out.save(png)
    social_preview(out)

    out.save(os.path.join(HERE, "icon.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

    if shutil.which("iconutil"):
        iconset = os.path.join(HERE, "icon.iconset")
        os.makedirs(iconset, exist_ok=True)
        for s in (16, 32, 128, 256, 512):
            out.resize((s, s), Image.LANCZOS).save(os.path.join(iconset, f"icon_{s}x{s}.png"))
            out.resize((s * 2, s * 2), Image.LANCZOS).save(os.path.join(iconset, f"icon_{s}x{s}@2x.png"))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(HERE, "icon.icns")], check=True)
        shutil.rmtree(iconset)
    else:
        print("iconutil not found (macOS only): icon.icns not updated", file=sys.stderr)


def social_preview(icon):
    w, h = 1280, 640
    img = gradient((w, h), (30, 27, 75), (8, 78, 99), 0.6).convert("RGBA")
    img.alpha_composite(icon.resize((400, 400), Image.LANCZOS), (70, 120))
    d = ImageDraw.Draw(img)
    font = "/System/Library/Fonts/HelveticaNeue.ttc"
    def f(size, index):
        try:
            return ImageFont.truetype(font, size, index=index)
        except OSError:
            return ImageFont.load_default(size)
    x = 500
    d.text((x, 190), "DB Manager", font=f(96, 1), fill=(255, 255, 255))
    d.text((x, 315), "Native PostgreSQL + Redis client", font=f(40, 0), fill=(224, 231, 255))
    d.text((x, 365), "for macOS and Windows", font=f(40, 0), fill=(224, 231, 255))
    d.text((x, 450), "C++20  \u00b7  Qt 6  \u00b7  Open source (MIT)", font=f(28, 10), fill=(129, 140, 248))
    img.convert("RGB").save(os.path.join(HERE, "social-preview.png"))


if __name__ == "__main__":
    main()
