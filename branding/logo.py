# SaturnProject logo: an almost black Saturn lit from the right (a crescent of light), a thin banded grey ring
# rising to the right, and the four controller symbols floating around it instead of moons.
# Usage: python logo.py <folder>  -> saturn-logo.png (transparent), saturn-logo-on-black/white.png, SaturnProject.ico
import sys, math
from PIL import Image, ImageDraw, ImageFilter, ImageChops

out = sys.argv[1]
S = 4096; C = S // 2              # drawn at 4x, then reduced to 1024
TILT = 20                         # the ring rises to the right (degrees, counter-clockwise)
R = 980                           # planet radius
RX = 1880; RATIO = 0.19           # ring semi-axis and flattening (perspective)


def L(): return Image.new("L", (S, S), 0)


def ellipse_ring(d, a_out, a_in, val):
    d.ellipse([C - a_out, C - a_out * RATIO, C + a_out, C + a_out * RATIO], fill=val)
    d.ellipse([C - a_in, C - a_in * RATIO, C + a_in, C + a_in * RATIO], fill=0)


def ring():
    # bands from outside in: (outer radius, inner radius, grey) as fractions of RX
    bands = [(1.000, 0.990, 230), (0.982, 0.948, 150), (0.946, 0.905, 205), (0.903, 0.893, 35),   # Cassini division
             (0.891, 0.820, 225), (0.818, 0.775, 170), (0.773, 0.735, 105), (0.733, 0.705, 55)]
    img = L()
    for a, b, v in bands:
        m = L(); ellipse_ring(ImageDraw.Draw(m), a * RX, b * RX, 255)
        img.paste(v, (0, 0), m)
    # light from the right: the left side of the ring stays darker
    grad = Image.linear_gradient("L").rotate(90).resize((S, S))
    if grad.getpixel((0, C)) > grad.getpixel((S - 1, C)): grad = grad.transpose(Image.FLIP_LEFT_RIGHT)
    grad = grad.point(lambda x: int(110 + 145 * (x / 255) ** 0.8))
    img = ImageChops.multiply(img, grad)
    front = L(); ImageDraw.Draw(front).rectangle([0, C, S, S], fill=255)
    back = ImageChops.invert(front)
    f = ImageChops.multiply(img, front).rotate(TILT, resample=Image.BICUBIC, center=(C, C))
    b = ImageChops.multiply(img, back).rotate(TILT, resample=Image.BICUBIC, center=(C, C))
    return b, f


def planet():
    body = L(); ImageDraw.Draw(body).ellipse([C - R, C - R, C + R, C + R], fill=255)
    # crescent of light: disc minus the same disc shifted left, blurred
    shifted = L(); off = int(R * 0.17)
    ImageDraw.Draw(shifted).ellipse([C - R - off, C - R + off // 3, C + R - off, C + R + off // 3], fill=255)
    crescent = ImageChops.subtract(body, shifted).filter(ImageFilter.GaussianBlur(26))
    crescent = ImageChops.multiply(crescent, body)
    # strongest at the right center, fading towards the poles
    fade = L(); ImageDraw.Draw(fade).ellipse([C - R * 0.2, C - R * 1.25, C + R * 2.2, C + R * 1.25], fill=255)
    fade = fade.filter(ImageFilter.GaussianBlur(260))
    crescent = ImageChops.multiply(crescent, fade)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste((7, 7, 7, 255), (0, 0), body)                               # the planet hides what is behind it
    img.paste((236, 236, 236, 255), (0, 0), crescent)
    # barely visible rim on the shadow side
    rim = L(); ImageDraw.Draw(rim).ellipse([C - R, C - R, C + R, C + R], outline=255, width=10)
    rim = ImageChops.multiply(rim.filter(ImageFilter.GaussianBlur(6)), ImageChops.invert(fade)).point(lambda x: x // 5)
    img.paste((120, 120, 120, 255), (0, 0), rim)
    return img


def ring_layer(mask):
    a = mask.point(lambda x: 255 if x else 0)
    return Image.merge("RGBA", (mask, mask, mask, a))


def glyphs(img):
    # instead of moons: (symbol, dx, dy from the center, radius)
    d = ImageDraw.Draw(img); col = (238, 238, 238, 255)
    for kind, dx, dy, r in (("triangle", -880, -1000, 92), ("square", 1080, -1180, 58), ("circle", -1700, -70, 60), ("cross", 1160, 700, 70)):
        x, y = C + dx, C + dy; w = max(14, int(r * 0.22))
        if kind == "triangle": d.polygon([(x, y - r * 1.05), (x - r, y + r * 0.75), (x + r, y + r * 0.75)], outline=col, width=w)
        elif kind == "circle": d.ellipse([x - r, y - r, x + r, y + r], outline=col, width=w)
        elif kind == "square": d.rectangle([x - r * 0.82, y - r * 0.82, x + r * 0.82, y + r * 0.82], outline=col, width=w)
        else:
            d.line([x - r * 0.8, y - r * 0.8, x + r * 0.8, y + r * 0.8], fill=col, width=w + 4)
            d.line([x - r * 0.8, y + r * 0.8, x + r * 0.8, y - r * 0.8], fill=col, width=w + 4)


back, front = ring()
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
img.alpha_composite(ring_layer(back))
img.alpha_composite(planet())
img.alpha_composite(ring_layer(front))
glyphs(img)
logo = img.resize((1024, 1024), Image.LANCZOS)
logo.save(out + r"\saturn-logo.png")
for name, colr in (("on-black", (0, 0, 0)), ("on-white", (245, 245, 245))):
    bg = Image.new("RGBA", (1024, 1024), colr + (255,)); bg.alpha_composite(logo); bg.convert("RGB").save(out + rf"\saturn-logo-{name}.png")
logo.save(out + r"\SaturnProject.ico", sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
print("ok")
