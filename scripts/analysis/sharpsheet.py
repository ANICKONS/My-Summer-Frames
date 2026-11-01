"""2x3 sheet of centre crops (640x360 at native size): DLSS Quality 0 / 5 / 10 on top, FSR Quality 0 / 5 / 10 below."""
import sys, os
from PIL import Image, ImageDraw

run, out = sys.argv[1], sys.argv[2]
rows = [["dlssq-0", "dlssq-5", "dlssq-10"], ["fsrq-0", "fsrq-5", "fsrq-10"]]
cw, ch = 640, 360
x0, y0 = int(sys.argv[3]) if len(sys.argv) > 3 else 960, int(sys.argv[4]) if len(sys.argv) > 4 else 540
sheet = Image.new("RGB", (cw * 3, ch * 2))
d = ImageDraw.Draw(sheet)
for r, row in enumerate(rows):
    for c, n in enumerate(row):
        im = Image.open(os.path.join(run, "shot_" + n + ".png")).convert("RGB").crop((x0, y0, x0 + cw, y0 + ch))
        sheet.paste(im, (c * cw, r * ch))
        d.text((c * cw + 8, r * ch + 6), n, fill=(255, 255, 0))
sheet.save(out)
