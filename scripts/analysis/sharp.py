"""Sharpness of the sharp*.json screenshots: mean |Laplacian| of the luma over the picture without the console
(bottom left) and the screen edges, plus the same over a 400x300 crop at the centre."""
import sys, os, glob
import numpy as np
from PIL import Image

run = sys.argv[1]
names = ["dlssq-0", "dlssq-5", "dlssq-10", "fsrq-0", "fsrq-5", "fsrq-10", "dlaa-0", "dlaa-10", "dlssq-s5-dlssfg2", "fsrq-s5-fsrfg"]


def lap(y):
    return np.abs(4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]).mean()


for n in names:
    p = os.path.join(run, "shot_" + n + ".png")
    if not os.path.exists(p):
        print(n, "missing")
        continue
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
    y = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
    h, w = y.shape
    top = y[100:1000, 850:w - 50]
    cy, cx = h // 2, w // 2
    centre = y[cy - 150:cy + 150, cx - 200:cx + 200]
    print(f"{n:18s} picture {lap(top):6.2f}  centre {lap(centre):6.2f}")
