import sys, glob, os
import numpy as np
from PIL import Image
run = sys.argv[1]
def load(p):
    a = np.asarray(Image.open(p).convert('L'), dtype=np.float32)
    return a
def lap(a):
    c = a[1:-1,1:-1]
    l = 4*c - a[:-2,1:-1] - a[2:,1:-1] - a[1:-1,:-2] - a[1:-1,2:]
    return float(np.mean(np.abs(l)))
print("== screenshots (mean |Laplacian|, centre 60%, mean level)")
for p in sorted(glob.glob(os.path.join(run, 'shot_*.png'))):
    a = load(p); h, w = a.shape
    c = a[int(h*.2):int(h*.8), int(w*.2):int(w*.8)]
    print(f"{os.path.basename(p):34s} {w}x{h}  sharp {lap(c):6.2f}  level {c.mean():6.1f}")
print("== jitter shots (sharpness a, b; |a-b| mean)")
d = os.path.join(run, 'dlss')
for n in ['off','pp','np','pn','nn']:
    a = load(os.path.join(d, f'shot_fsrq_{n}_a.png')); b = load(os.path.join(d, f'shot_fsrq_{n}_b.png'))
    h, w = a.shape
    ca = a[int(h*.2):int(h*.8), int(w*.2):int(w*.8)]; cb = b[int(h*.2):int(h*.8), int(w*.2):int(w*.8)]
    print(f"{n:4s} sharp {lap(ca):6.2f} {lap(cb):6.2f}  diff {np.mean(np.abs(ca-cb)):6.3f}")
