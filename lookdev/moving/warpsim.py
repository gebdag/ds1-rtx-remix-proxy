"""Warp simulator: plays captures into a live host through the game's stream, one after another, the way warping around streams new
areas. Stands in for the game (no game process needed). Ctrl+C or the host's death stops it.

    warpsim.py <seconds per capture> [--days N] [--seed N] [--match TEXT] [--count N]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts")))
import ds1paths  # noqa: E402

import argparse
import ctypes
import mmap
import numpy as np
import os
import random
import struct
import sys
import time
from pathlib import Path

CAPS = Path(ds1paths.GAME + r"\DATA\rtx_comp\lookdev")
RING = 128 << 20
NAME = "Local\\ds1_lookdev_stream"
OFF_WRITE, OFF_READ, OFF_START, OFF_SESSION, OFF_EPOCH, OFF_CONS, OFF_PROD, OFF_BEAT, OFF_WRITTEN, OFF_CONSUMED = 16, 24, 32, 40, 44, 48, 52, 56, 60, 64
REC_DESTROY = 12
REC_PRESENT = None  # read from the capture format below


def rec_types():
    src = Path(ds1paths.REPO + r"\src\shared\lookdev\capture_format.hpp").read_text(encoding="utf-8")
    import re
    block = src[src.index("enum record_type"):]
    block = block[:block.index("};")]
    names, v = {}, -1
    for line in block.splitlines():
        m = re.match(r"\s*(REC_\w+)\s*(?:=\s*(\d+))?\s*,?", line)
        if m:
            v = int(m.group(2)) if m.group(2) else v + 1
            names[m.group(1)] = v
    return names


T = rec_types()
assert T["REC_DESTROY"] == REC_DESTROY, T["REC_DESTROY"]
RESOURCE = {T[k] for k in ("REC_TEXTURE", "REC_CUBE_TEXTURE", "REC_VOLUME_TEXTURE", "REC_SURFACE", "REC_VERTEX_BUFFER", "REC_INDEX_BUFFER",
                           "REC_VERTEX_SHADER", "REC_PIXEL_SHADER", "REC_VERTEX_DECL")}
BEGIN, PRESENT = T["REC_STREAM_BEGIN"], T["REC_PRESENT"]
DATA = {T["REC_TEXTURE_DATA"], T["REC_SURFACE_DATA"], T["REC_BUFFER_DATA"], T["REC_SURFACE_LEVEL"], T["REC_BACKBUFFER"]} | RESOURCE
SKIP = {T["REC_REFERENCE_IMAGE"]}
HEADER_FIELDS = 21


def parse(path):
    data = path.read_bytes()
    header_size = struct.unpack_from("<I", data, 8)[0]
    recs, pos = [], header_size
    while pos + 8 <= len(data):
        t, s = struct.unpack_from("<II", data, pos)
        pos += 8
        if pos + s > len(data):
            break
        recs.append((t, pos, s))
        pos += s
    return data, header_size, recs


class Stream:
    def __init__(self):
        self.m = mmap.mmap(-1, 4096 * 0 + 128 + RING, tagname=NAME) if False else None
        while self.m is None:
            try:
                h = ctypes.windll.kernel32.OpenFileMappingW(0xF001F, False, NAME)
                if h:
                    ctypes.windll.kernel32.CloseHandle(h)
                    probe = mmap.mmap(-1, 128, tagname=NAME)
                    header = struct.unpack_from("<I", probe, 8)[0]
                    if header:
                        self.m = mmap.mmap(-1, header + RING, tagname=NAME)
                        self.base = header
                    probe.close()
            except OSError:
                pass
            time.sleep(0.2)
        self.pos = self.q(OFF_WRITE)
        self.frames = 0

    def q(self, off):
        return struct.unpack_from("<q", self.m, off)[0]

    def u(self, off):
        return struct.unpack_from("<I", self.m, off)[0]

    def put_u(self, off, v):
        struct.pack_into("<I", self.m, off, v)

    def start(self):
        struct.pack_into("<q", self.m, OFF_START, self.pos)
        self.put_u(OFF_PROD, os.getpid())
        self.put_u(OFF_WRITTEN, 0)
        self.frames = 0
        self.put_u(OFF_SESSION, self.u(OFF_SESSION) + 1)

    def alive(self):
        return self.u(OFF_CONS) != 0 and (ctypes.windll.kernel32.GetTickCount() - self.u(OFF_BEAT)) & 0xFFFFFFFF < 15000

    def write(self, blob):
        """blob: one or more whole records. Waits for ring room; publishes at the end."""
        n = len(blob)
        assert n <= RING, n
        while RING - (self.pos - self.q(OFF_READ)) < n:
            if not self.alive():
                raise SystemExit("host gone")
            time.sleep(0.002)
        o = self.pos % RING
        first = min(n, RING - o)
        self.m[self.base + o:self.base + o + first] = blob[:first]
        if first < n:
            self.m[self.base:self.base + n - first] = blob[first:]
        self.pos += n
        struct.pack_into("<q", self.m, OFF_WRITE, self.pos)

    def wait_consumed(self, behind):
        while self.frames - self.u(OFF_CONSUMED) > behind:
            if not self.alive():
                raise SystemExit("host gone")
            time.sleep(0.002)


def rec(t, payload):
    return struct.pack("<II", t, len(payload)) + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", type=float)
    ap.add_argument("--days", type=float, default=6)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--match", default="")
    ap.add_argument("--count", type=int, default=0)
    ap.add_argument("--spawn", type=float, default=0.0, help="quads live and die smoothly at this rate; batches hold the living ones packed to the front")
    ap.add_argument("--move", type=float, default=0.0, help="moves each 48-byte sprite quad by this many metres and varies its alpha per frame, as live particles do")
    ap.add_argument("--churn", type=int, default=0, help="re-upload this many small vertex buffers every frame with changed contents, as animation does")
    ap.add_argument("--budget", type=int, default=0, help="MB of contents per session, as the game's kMaxSessionContents")
    a = ap.parse_args()
    now = time.time()
    caps = [p for p in CAPS.glob("*.ds1cap") if now - p.stat().st_mtime < a.days * 86400 and a.match in p.name]
    random.Random(a.seed).shuffle(caps)
    if a.count:
        caps = caps[:a.count]
    print(len(caps), "captures", flush=True)
    s = Stream()
    print("host attached", flush=True)
    time.sleep(1)
    seen = set()
    contents = 0
    started = False
    version = None
    for n, cap in enumerate(caps):
        data, hs, recs = parse(cap)
        v = struct.unpack_from("<I", data, 4)[0]
        if version is None:
            version = v
        if v != version:
            print("skip", cap.name, "version", v, flush=True)
            continue
        if not started:
            s.start()
            s.write(rec(BEGIN, data[:hs]))
            started = True
        if a.budget and contents > a.budget << 20:
            while s.q(OFF_READ) < s.pos:
                time.sleep(0.01)
            s.start()
            s.write(rec(BEGIN, data[:hs]))
            print(f"new session after {contents >> 20} MB", flush=True)
            seen.clear()
            contents = 0
        t0 = time.time()
        first_cmd = next(i for i, (t, _, _) in enumerate(recs) if t not in DATA and t not in SKIP)
        chunk = []
        size = 0

        def flush():
            nonlocal chunk, size
            if chunk:
                s.write(b"".join(chunk))
                chunk, size = [], 0

        destroyed = 0
        for t, off, sz in recs:
            if t in SKIP or t == PRESENT or t not in DATA:
                continue
            if t in RESOURCE or t == T["REC_SURFACE_LEVEL"]:
                rid = data[off:off + 8]
                if rid in seen:
                    r = rec(REC_DESTROY, rid)
                    chunk.append(r)
                    size += len(r)
                    destroyed += 1
                seen.add(rid)
            r = data[off - 8:off + sz]
            if t in (T["REC_TEXTURE_DATA"], T["REC_SURFACE_DATA"], T["REC_BUFFER_DATA"]):
                contents += sz
            if size + len(r) > 32 << 20:
                flush()
            chunk.append(r)
            size += len(r)
        flush()
        frame = b"".join(data[off - 8:off + sz] for t, off, sz in recs if t not in DATA and t not in SKIP and t != PRESENT) + rec(PRESENT, struct.pack("<II", 0, 0))
        upload = time.time() - t0
        churn = []
        up_ranges = []
        movers = []
        if a.move > 0:
            at = 0
            rng = np.random.default_rng(7)
            frame_bytes = np.frombuffer(bytearray(frame), np.uint8).copy()
            dtype = np.dtype({"names": ["pos", "colour"], "formats": [("<f4", 3), ("u1", 4)], "offsets": [0, 24], "itemsize": 48})
            for t, off, sz in recs:
                if t in DATA or t in SKIP or t == PRESENT:
                    continue
                if t == T["REC_DRAW_INDEXED_UP"]:
                    v = struct.unpack_from("<8I", data, off)
                    if v[5] == 48 and v[4] == 101 and v[6] % 12 == 0 and v[6] // 2 >= 6:
                        vstart = at + 8 + 32 + v[6]
                        view = np.ndarray(v[7] // 48, dtype, frame_bytes, vstart)
                        ids = np.ndarray(v[6] // 2, "<u2", frame_bytes, at + 8 + 32)
                        used = v[3] * 3
                        groups = ids[:used].reshape(-1, 6).copy()
                        n = len(groups)
                        # The quad each vertex slot belongs to.
                        slot_quad = np.full(len(view), -1)
                        for q in range(n):
                            slot_quad[groups[q]] = q
                        movers.append((view, view.copy(), ids, groups, slot_quad, rng.uniform(0, 6.28, (n, 3)), rng.uniform(1.0, 4.0, n), at + 8 + 12))
                at += 8 + sz
            print(len(movers), "sprite batches moved per frame", flush=True)
        if a.churn:
            # Sprite draws carry their vertices: where those lie in the frame's bytes. Moving them gives new geometry each frame, as live effects do.
            at = 0
            for t, off, sz in recs:
                if t in DATA or t in SKIP or t == PRESENT:
                    continue
                if t == T["REC_VS_CONST_F"] and struct.unpack_from("<I", data, off)[0] >= 8:
                    # Bone palettes: skinned characters animate, so their expanded vertices hash as new geometry each frame.
                    up_ranges.append((at + 8 + 8, sz - 8))
                elif t == T["REC_DRAW_UP"]:
                    n = struct.unpack_from("<IIII", data, off)[3]
                    up_ranges.append((at + 8 + 16, n))
                elif t == T["REC_DRAW_INDEXED_UP"]:
                    v = struct.unpack_from("<8I", data, off)
                    up_ranges.append((at + 8 + 32 + v[6], v[7]))
                at += 8 + sz
            frame_bytes = np.frombuffer(bytearray(frame), np.uint8).copy()
            print(len(up_ranges), "sprite draws moved per frame", flush=True)
            vbs = {data[off:off + 8] for t, off, sz in recs if t == T["REC_VERTEX_BUFFER"]}
            for t, off, sz in recs:
                if t == T["REC_BUFFER_DATA"] and data[off:off + 8] in vbs:
                    bid, flags, boff, size = struct.unpack_from("<QIII", data, off)
                    if boff == 0 and 256 <= size <= 32768 and len(churn) < a.churn:
                        churn.append((bid, flags | 0x2000, size, np.frombuffer(data, np.uint8, size, off + 20).copy()))
            print(len(churn), "buffers churned per frame", flush=True)
        frames = 0
        end = time.time() + a.seconds
        while time.time() < end:
            s.wait_consumed(2)
            for bid, flags, size, bytes_ in churn:
                changed = bytes_.copy()
                # The lowest bits of each dword: float positions move by an ulp, enough to hash as new geometry.
                changed[0::4] ^= (frames % 251) + 1
                s.write(rec(T["REC_BUFFER_DATA"], struct.pack("<QIII", bid, flags, 0, size) + changed.tobytes()))
            if movers:
                now = time.time()
                for view, base, ids, groups, slot_quad, phase, speed, count_at in movers:
                    # As the game's batches: vertices in fixed slots, each quad living and dying smoothly from alpha 0 to its own
                    # and back, moving, and the index list holding the living quads sorted by depth, rewritten every frame.
                    live_slots = slot_quad >= 0
                    q = slot_quad[live_slots]
                    moved = base[live_slots].copy()
                    moved["pos"] = moved["pos"] + a.move * np.sin(now * speed[q, None] + phase[q])
                    if a.spawn > 0:
                        life = (now * speed * a.spawn + phase[:, 0]) % 1.0
                        envelope = np.sin(life * np.pi) ** 2
                    else:
                        envelope = 0.85 + 0.15 * np.sin(now * speed * 1.7 + phase[:, 0])
                    moved["colour"][:, 3] = (moved["colour"][:, 3] * envelope[q]).astype(np.uint8)
                    view[live_slots] = moved
                    alive = np.nonzero(envelope > 0.01)[0]
                    depth = view["pos"][groups[alive, 0], 2]
                    order = alive[np.argsort(depth)]
                    ids[:len(order) * 6] = groups[order].reshape(-1)
                    frame_bytes[count_at:count_at + 4] = np.frombuffer(struct.pack("<I", len(order) * 2), np.uint8)
                frame = frame_bytes.tobytes()
            if up_ranges:
                for start, n in up_ranges:
                    frame_bytes[start:start + n:4] ^= 1 + frames % 2
                frame = frame_bytes.tobytes()
            s.write(frame)
            s.frames += 1
            s.put_u(OFF_WRITTEN, s.frames)
            frames += 1
            time.sleep(1 / 60)
        print(f"{n + 1}/{len(caps)} {cap.name}: upload {upload:.1f} s, destroyed {destroyed}, {frames} frames, {len(seen)} ids, frame {len(frame) >> 10} KB", flush=True)
    print("done", flush=True)
    time.sleep(3)
    s.put_u(OFF_PROD, 0)


main()
