"""Set Remix options on the running lookdev host (no restart): python live.py key=value [key=value ...]

Keeps a persistent base block (the header of remix_live.txt, up to the '# --- live' line) and
replaces everything after it with the given overrides; `python live.py --clear` drops them.
The proxy in the host applies the file whenever it changes (atmosphere.cpp apply_live_options).
"""
import os
import sys
import time
from pathlib import Path

# The host's folder: DATA, or the nv set's own host folder (DS1_LOOKDEV_GAME_DIR, see lookdev.py).
GAME_DIR = Path(os.environ.get("DS1_LOOKDEV_GAME_DIR", r"<game dir>\DATA"))
LIVE = GAME_DIR / "rtx_comp" / "lookdev" / "remix_live.txt"
MARK = "# --- live"


def main() -> int:
    text = LIVE.read_text(encoding="utf-8") if LIVE.exists() else ""
    base = text.split(MARK)[0].rstrip("\n") + "\n"
    overrides = [] if "--clear" in sys.argv else [a for a in sys.argv[1:] if "=" in a]
    lines = [base, MARK + " (python live.py)\n"]
    lines += [f"{k.strip()} = {v.strip()}\n" for k, v in (a.split("=", 1) for a in overrides)]
    LIVE.write_text("".join(lines), encoding="utf-8")
    # The host polls every 6 frames; one frame of the new state is in flight after that.
    time.sleep(0.4)
    print("".join(lines[1:]).strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
