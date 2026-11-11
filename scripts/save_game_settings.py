"""Copies the light and area settings made in the game into this repo, so they are in git and not only in the game folder.

    python scripts/save_game_settings.py             game -> repo (then commit)
    python scripts/save_game_settings.py --restore   repo -> game, each replaced file kept beside it as .bak-<date>-restore
    python scripts/save_game_settings.py --check     only says what differs

Also kept: DATA/rtx_comp/emissive_scales.txt (the runtime's Dark Souls menu > Emissive per texture) as assets/emissive_scales.txt.

What is kept:
    DATA\\rtx_comp\\settings\\area_overrides.ini                     assets/area_overrides.ini
    DATA\\rtx_comp\\lights\\*.txt (map tables, custom lights, ...)   assets/lights/
    DATA\\rtx_comp\\settings\\rtx_comp\\lights\\light_overrides.txt    assets/light_overrides.txt

The F4 menu writes light files without the repo's comments. A file whose values are the same as the repo's is left
alone; in one that differs, the repo's comment lines stay and each light line is replaced by the game's line of the same
name (deleted lights go, new ones are added at the end).
"""
import argparse
import shutil
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GAME = Path(r"<game dir>\DATA\rtx_comp")
if (REPO / "deploy.conf").exists():
    for line in (REPO / "deploy.conf").read_text(encoding="utf-8").splitlines():
        if line.strip() and Path(line.strip().strip('"')).is_dir():
            GAME = Path(line.strip().strip('"')) / "rtx_comp"
            break


def text_of(path):
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def values(text, comment):
    """The lines that carry values, spacing and trailing comments aside."""
    out = []
    for line in text.split("\n"):
        line = line.split(" # ")[0].strip() if comment == "#" else line.strip()
        if line and not line.startswith(comment):
            out.append(" ".join(line.split()))
    return out


def light_name(line):
    f = line.split()
    return f[9] if len(f) >= 10 and not line.lstrip().startswith("#") else None


def merge_lights(repo_text, game_text):
    """The repo's file with the game's values: comments kept, lights replaced by name."""
    game = [l for l in game_text.split("\n") if light_name(l)]
    by_name = {}
    for l in game:
        if light_name(l) in by_name:
            return None
        by_name[light_name(l)] = l
    out, used = [], set()
    for line in repo_text.split("\n"):
        name = light_name(line)
        if name is None:
            if line.strip() and not line.lstrip().startswith("#"):
                return None
            out.append(line)
        elif name in by_name and name not in used:
            used.add(name)
            out.append(by_name[name])
    while out and not out[-1].strip():
        out.pop()
    out += [l for l in game if light_name(l) not in used]
    return "\n".join(out) + "\n"


def pairs():
    yield GAME / "settings" / "area_overrides.ini", REPO / "assets" / "area_overrides.ini", ";"
    yield GAME / "settings" / "rtx_comp" / "lights" / "light_overrides.txt", REPO / "assets" / "light_overrides.txt", "#"
    yield GAME / "emissive_scales.txt", REPO / "assets" / "emissive_scales.txt", "#"
    names = {p.name for p in (GAME / "lights").glob("*.txt")} | {p.name for p in (REPO / "assets" / "lights").glob("*.txt")}
    for name in sorted(names):
        if name != "light_overrides.txt":   # the one in lights\ is from before the settings folder; nothing reads it
            yield GAME / "lights" / name, REPO / "assets" / "lights" / name, "#"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    stamp = time.strftime(".bak-%Y%m%d-%H%M-restore")
    changed = 0
    for game, repo, comment in pairs():
        src, dst = (repo, game) if args.restore else (game, repo)
        if not src.exists():
            if dst.exists():
                print(f"only in the {'game' if args.restore else 'repo'}: {dst.name}")
            continue
        same = dst.exists() and values(text_of(src), comment) == values(text_of(dst), comment)
        if same:
            continue
        changed += 1
        print(("would copy " if args.check else "copied ") + f"{src} -> {dst}")
        if args.check:
            continue
        text = text_of(src)
        if not args.restore and dst.exists() and game.parent.name == "lights" and ".custom" in game.name:
            text = merge_lights(text_of(dst), text) or text
        if args.restore and dst.exists():
            shutil.copy2(dst, str(dst) + stamp)
        dst.parent.mkdir(parents=True, exist_ok=True)
        # Each side keeps its own line endings (the menu writes CRLF, the repo is LF).
        crlf = b"\r\n" in (dst if dst.exists() else src).read_bytes()
        dst.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
    print(f"{changed} file(s) differ" if args.check else f"{changed} file(s) copied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
