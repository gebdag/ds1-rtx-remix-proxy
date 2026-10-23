"""python crop.py x0 y0 x1 y1 [tag] -> snaps/crop.png: game above Remix, same region (fractions 0-1 of the frame)."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw
import numpy as np
d = Path(__file__).resolve().parent / "snaps"
x0, y0, x1, y1 = map(float, sys.argv[1:5])
tag = f"_{sys.argv[5]}" if len(sys.argv) > 5 else ""
g = Image.open(d / f"game{tag}.png").convert("RGB")
r = Image.open(d / f"remix{tag}.png").convert("RGB").resize(g.size, Image.LANCZOS)
box = (int(x0 * g.width), int(y0 * g.height), int(x1 * g.width), int(y1 * g.height))
a, b = g.crop(box), r.crop(box)
scale = min(1.0, 1200 / a.width)
a = a.resize((int(a.width * scale), int(a.height * scale)), Image.LANCZOS)
b = b.resize(a.size, Image.LANCZOS)
out = Image.new("RGB", (a.width, 2 * a.height + 4))
out.paste(a, (0, 0)); out.paste(b, (0, a.height + 4))
out.save(d / "crop.png")
ga, ra = np.asarray(g.crop(box), np.float32), np.asarray(r.crop(box), np.float32)
print("game mean", ga.reshape(-1, 3).mean(0).round(1), "remix mean", ra.reshape(-1, 3).mean(0).round(1))
