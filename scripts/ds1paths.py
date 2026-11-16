"""Folders the DS1 tools work with, from environment variables or from paths.local.ini at the repo root.

paths.local.ini is not tracked; copy paths.example.ini and fill it in, or set the variables instead:

  DS1_GAME_DIR          the game folder that holds DATA\\ (e.g. <Steam>\\steamapps\\common\\Dark Souls Prepare to Die Edition)
  DS1_LOOKDEV_GAME_DIR  the DATA folder the lookdev host runs from (default: DS1_GAME_DIR\\DATA)
  DS1_RUNTIME_DIR       a checkout of the RTX Remix runtime fork (lookdev/redeploy.py)
  DS1_PBR_TOOLS         Vibe Reverse Engineering's pbr_tools folder (gen_foliage, gen_glass, gen_painted_meshes,
                        gen_texture_fixes)
  DS1_ANALYSIS_DIR      the folder with the effect analysis notes gen_ffx_tables reads (or pass it as an argument)

A path that is not set reads "<set NAME ...>", so a tool that needs it stops with that text in its error.
"""
import configparser
import os
from pathlib import Path

REPO = str(Path(__file__).resolve().parents[1])
SCRIPTS = str(Path(__file__).resolve().parent)

_ini = configparser.ConfigParser()
_ini.optionxform = str
_ini.read(os.path.join(REPO, "paths.local.ini"), encoding="utf-8")


def get(name, default=None):
    value = os.environ.get(name) or _ini.get("paths", name, fallback="")
    if value:
        return value.rstrip("\\/")
    return default if default is not None else f"<set {name} in the environment or paths.local.ini>"


GAME = get("DS1_GAME_DIR")
LOOKDEV_GAME = get("DS1_LOOKDEV_GAME_DIR", GAME + "\\DATA")
RUNTIME = get("DS1_RUNTIME_DIR")
PBR_TOOLS = get("DS1_PBR_TOOLS")
ANALYSIS = get("DS1_ANALYSIS_DIR")
