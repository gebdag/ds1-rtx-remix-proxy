"""Seed index.db `strings` and `segments` from the PE image.

The Ghidra export path populates funcs/names/xrefs/blocks but not strings, and
string search is essential for locating draw-plan scripts, debug-menu labels and
shader/material names. This scans the mapped image directly (ASCII + UTF-16LE)
and writes rows with source='pescan', so `retools.query` can serve them without
reopening Ghidra.

Usage:
  python patches/DarkSouls/scripts/seed_strings.py <binary> <index.db> [min_len]
"""

from __future__ import annotations

import re
import sqlite3
import sys
from pathlib import Path

import pefile

ASCII_RE = rb"[\x20-\x7e]{%d,}"
UTF16_RE = rb"(?:[\x20-\x7e]\x00){%d,}"


def main(argv: list[str]) -> int:
    binary, db_path = Path(argv[1]), Path(argv[2])
    min_len = int(argv[3]) if len(argv) > 3 else 5

    pe = pefile.PE(str(binary), fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase

    conn = sqlite3.connect(db_path)
    with conn:
        conn.execute("DELETE FROM strings WHERE source = 'pescan'")
        conn.execute("DELETE FROM segments WHERE source = 'pescan'")

        rows: dict[int, tuple] = {}
        for section in pe.sections:
            name = section.Name.rstrip(b"\x00").decode("ascii", "replace")
            start = image_base + section.VirtualAddress
            data = section.get_data()
            conn.execute(
                "INSERT INTO segments (start_ea, end_ea, name, class, perm, source) VALUES (?, ?, ?, ?, ?, 'pescan')",
                (start, start + section.Misc_VirtualSize, name, None,
                 section.Characteristics))

            for match in re.finditer(ASCII_RE % min_len, data):
                addr = start + match.start()
                text = match.group().decode("ascii", "replace")
                rows[addr] = (addr, len(text), "string", "ascii", text, "pescan")

            for match in re.finditer(UTF16_RE % min_len, data):
                addr = start + match.start()
                if addr in rows:
                    continue
                text = match.group().decode("utf-16-le", "replace")
                rows[addr] = (addr, len(text), "unicode", "utf-16le", text, "pescan")

        conn.executemany(
            "INSERT OR REPLACE INTO strings (address, length, type, encoding, content, source) "
            "VALUES (?, ?, ?, ?, ?, ?)", rows.values())

    n = conn.execute("SELECT COUNT(*) FROM strings WHERE source='pescan'").fetchone()[0]
    print(f"seeded {n} strings from {binary.name} into {db_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
