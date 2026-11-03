"""Flicker on a stream of moving sprites, with one suspect off at a time. Needs a live host with warpsim --move running.
    flick.py <tag> label="settings" [label="settings" ...]
Grabs frames from the host window, frame-to-frame luminance change inside the sprite region (top 60% of the frame, right of the HUD)."""
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

LOOKDEV = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(LOOKDEV))
os.chdir(LOOKDEV)
import shot  # noqa: E402

HERE = Path(__file__).parent
LUMA = np.array([0.2126, 0.7152, 0.0722])
tag = sys.argv[1]
GRABS = 30


def measure(label):
    frames = []
    for _ in range(GRABS):
        img = shot.grab().resize((640, 360), Image.BOX)
        frames.append(np.asarray(img, np.float32))
        time.sleep(0.06)
    a = np.stack(frames) @ LUMA
    region = a[:, 40:216, 200:640]
    d = np.abs(np.diff(region, axis=0))
    # The swing between successive frames, and how much of it is a fast flip rather than the sprites' own slow motion:
    # a flip goes up and comes back (the two steps of opposite sign), motion keeps its direction.
    signed = np.diff(region, axis=0)
    flip = np.minimum(np.abs(signed[:-1]), np.abs(signed[1:])) * (np.sign(signed[:-1]) != np.sign(signed[1:]))
    title = shot.host_title()
    fps = title.split(" fps")[0].split()[-1] if " fps" in title else "?"
    print(f"{label:16} {fps:>5} fps mean {region.mean():6.1f}  step mean {d.mean():5.2f} p99 {np.percentile(d, 99):6.1f}  flip mean {flip.mean():5.2f} p99 {np.percentile(flip, 99):6.1f}  pixels flipping >40: {(flip > 40).mean() * 100:5.2f}%", flush=True)
    s = Image.new("RGB", (640 * 3, 360))
    for i in range(3):
        s.paste(Image.fromarray(frames[i * 4].astype(np.uint8)), (640 * i, 0))
    s.save(HERE / f"flick_{tag}_{label}.png")


for arg in sys.argv[2:]:
    label, settings = arg.split("=", 1)
    subprocess.run([sys.executable, "live.py", *settings.split()], capture_output=True)
    time.sleep(6)
    measure(label)
