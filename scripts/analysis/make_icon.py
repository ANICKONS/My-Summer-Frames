"""Square mod icon from banner_github.png: the title in two lines ("My Summer" / "Frames") over the stack of frames,
on the banner's own forest, blurred. Usage: _make_icon.py banner.png out.png [size]"""
import sys
import numpy as np
from PIL import Image, ImageFilter, ImageEnhance

src = Image.open(sys.argv[1]).convert("RGB")
out_path = sys.argv[2]
S = 1024
a = np.asarray(src).astype(np.int32)
r, g, b = a[..., 0], a[..., 1], a[..., 2]
H, W = r.shape
luma = (r * 299 + g * 587 + b * 114) // 1000


def letters(x0, x1, y0, y1, fill):
    """RGBA cut of one title part: the letters, their dark outline and shadow, nothing of the background."""
    m = np.zeros((H, W), bool)
    m[y0:y1, x0:x1] = fill[y0:y1, x0:x1]
    fm = Image.fromarray((m * 255).astype(np.uint8))
    near = np.asarray(fm.filter(ImageFilter.MaxFilter(5))) > 0           # anti-aliased letter edges
    # Outline (black edge) and drop shadow (neutral near-black): grown out from the letters through connected
    # pixels only, so the dark olive trees around them stay out.
    dark = (luma < 34) & (np.abs(r - g) < 7) & (b - r > -7) & (b - r < 9)
    # The outline is ~3 px and the shadow ~6 px on the banner: nothing farther than 12 px from a letter belongs to it.
    band = np.asarray(fm.filter(ImageFilter.MaxFilter(25))) > 0
    allowed = Image.fromarray((((dark & band) | near) * 255).astype(np.uint8))
    grow = Image.fromarray((near * 255).astype(np.uint8))
    for _ in range(26):
        grow = Image.fromarray(np.minimum(np.asarray(grow.filter(ImageFilter.MaxFilter(3))), np.asarray(allowed)))
    keep = np.asarray(grow) > 0
    k = Image.fromarray((keep * 255).astype(np.uint8))
    k = k.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))   # close pinholes
    k = k.filter(ImageFilter.GaussianBlur(1.2))
    rgba = src.copy().convert("RGBA")
    rgba.putalpha(k)
    box = k.getbbox()
    return rgba.crop(box)


yellow = (r > 200) & (g > 150) & (b < 90) & (r - b > 140)
red = (r > 170) & (g < 90) & (b < 80)
line1 = letters(100, 880, 30, 300, yellow)
line2 = letters(880, 1400, 20, 230, red)

# Background: the stack's own surroundings, blurred, so the feathered stack blends in; darker at the top for the title.
bg = src.crop((462, 228, 1628, 728))
scale = max(S / bg.width, S / bg.height)
bg = bg.resize((int(bg.width * scale) + 1, int(bg.height * scale) + 1), Image.LANCZOS)
bg = bg.crop(((bg.width - S) // 2, (bg.height - S) // 2, (bg.width - S) // 2 + S, (bg.height - S) // 2 + S))
bg = bg.filter(ImageFilter.GaussianBlur(28))
shade = np.linspace(0.55, 0.95, S).reshape(S, 1, 1)
bg = Image.fromarray(np.clip(np.asarray(bg).astype(np.float32) * shade, 0, 255).astype(np.uint8))
canvas = bg.convert('RGBA')

# The stack of frames with soft edges, at the bottom.
stack = src.crop((462, 228, 1628, 728)).convert("RGBA")
fw = 1000
stack = stack.resize((fw, int(stack.height * fw / stack.width)), Image.LANCZOS)
alpha = Image.new("L", stack.size, 255)
feather = 34
ar = np.asarray(alpha).astype(np.float32)
yy, xx = np.mgrid[0:stack.height, 0:stack.width]
d = np.minimum(np.minimum(xx, stack.width - 1 - xx), np.minimum(yy, stack.height - 1 - yy)).astype(np.float32)
top = np.clip(yy / 110.0, 0, 1)
ar = np.clip(d / feather, 0, 1) * top * 255
stack.putalpha(Image.fromarray(ar.astype(np.uint8)))
sy = S - stack.height - 26
canvas.alpha_composite(stack, ((S - fw) // 2, sy))

# The title: same scale for both lines, "My Summer" wide, "Frames" under it, a little overlap as on the banner.
k = 940 / line1.width
l1 = line1.resize((int(line1.width * k), int(line1.height * k)), Image.LANCZOS)
l2 = line2.resize((int(line2.width * k), int(line2.height * k)), Image.LANCZOS)
y1 = 28
canvas.alpha_composite(l1, ((S - l1.width) // 2, y1))
y2 = y1 + l1.height - int(l1.height * 0.16)
canvas.alpha_composite(l2, ((S - l2.width) // 2 + 40, y2))

size = int(sys.argv[3]) if len(sys.argv) > 3 else S
img = canvas.convert("RGB")
if size != S:
    img = img.resize((size, size), Image.LANCZOS)
img.save(out_path, optimize=True)
print(out_path, img.size, "title", l1.size, l2.size, "stack at", sy, "frames", stack.size)
