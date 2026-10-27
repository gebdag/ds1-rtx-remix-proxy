"""Draw-level analysis of a lookdev capture (.ds1cap).

  python analyze.py draws <capture> [--ps SUBSTR] [--vs SUBSTR] [--tex SUBSTR] [--blend] [--range A-B]
      one line per draw: shaders (game names), blend / alpha test / depth state,
      textures (TPF names), render target
  python analyze.py draw <capture> <index>
      everything about one draw: full state, vertex declaration, non-zero shader
      constants
  python analyze.py shader <capture|-> <name or substring>
      D3DX disassembly of a game shader (from DATA\\unpacked\\shader)
  python analyze.py shaders <capture>
      every shader the capture uses, with its game name and draw count
  python analyze.py verts <capture> <index> [--max N]
      vertices of a UP draw, decoded with its declaration
  python analyze.py texture <capture> <name|0xid> <out.png>
      a captured texture's top level as PNG

Shaders are named by matching their bytecode with the game's unpacked .fpo/.vpo
files, so a family of draws can be found across the whole game, not by address.
"""
import argparse
import collections
import ctypes
import hashlib
import struct
import sys
from pathlib import Path

import os  # noqa: E402
# DATA, or the nv set's own host folder (DS1_LOOKDEV_GAME_DIR, see lookdev.py).
GAME_DIR = Path(os.environ.get("DS1_LOOKDEV_GAME_DIR", r"<game dir>\DATA"))
SHADER_DIR = GAME_DIR / "unpacked" / "shader"
CAPTURE_DIR = GAME_DIR / "rtx_comp" / "lookdev"

RS_NAMES = {7: "ZENABLE", 8: "FILLMODE", 9: "SHADEMODE", 14: "ZWRITEENABLE", 15: "ALPHATESTENABLE", 16: "LASTPIXEL",
            19: "SRCBLEND", 20: "DESTBLEND", 22: "CULLMODE", 23: "ZFUNC", 24: "ALPHAREF", 25: "ALPHAFUNC",
            26: "DITHERENABLE", 27: "ALPHABLENDENABLE", 28: "FOGENABLE", 29: "SPECULARENABLE", 34: "FOGCOLOR",
            35: "FOGTABLEMODE", 48: "RANGEFOGENABLE", 52: "STENCILENABLE", 60: "TEXTUREFACTOR", 137: "LIGHTING",
            152: "CLIPPLANEENABLE", 168: "COLORWRITEENABLE", 171: "BLENDOP", 174: "SCISSORTESTENABLE",
            175: "SLOPESCALEDEPTHBIAS", 194: "SRGBWRITEENABLE", 195: "DEPTHBIAS", 206: "SEPARATEALPHABLENDENABLE",
            207: "SRCBLENDALPHA", 208: "DESTBLENDALPHA", 209: "BLENDOPALPHA"}
BLEND = {1: "0", 2: "1", 3: "SrcC", 4: "1-SrcC", 5: "SrcA", 6: "1-SrcA", 7: "DstA", 8: "1-DstA", 9: "DstC",
         10: "1-DstC", 11: "SrcAsat", 14: "Factor", 15: "1-Factor"}
BLENDOP = {1: "+", 2: "-", 3: "rev-", 4: "min", 5: "max"}
CMP = {1: "never", 2: "<", 3: "==", 4: "<=", 5: ">", 6: "!=", 7: ">=", 8: "always"}
DECLTYPE = {0: "F1", 1: "F2", 2: "F3", 3: "F4", 4: "D3DCOLOR", 5: "UB4", 6: "S2", 7: "S4", 8: "UB4N", 9: "S2N",
            10: "S4N", 11: "US2N", 12: "US4N", 15: "F16x2", 16: "F16x4"}
DECLUSAGE = {0: "POS", 1: "BLENDW", 2: "BLENDI", 3: "NORMAL", 4: "PSIZE", 5: "TEXCOORD", 6: "TANGENT",
             7: "BINORMAL", 9: "POST", 10: "COLOR", 11: "FOG", 12: "DEPTH"}


# ---- capture parsing ----

def records(path):
    data = Path(path).read_bytes()
    magic, version, header_size = struct.unpack_from("<III", data, 0)
    if magic != 0x43315344:
        sys.exit("not a lookdev capture")
    pos = header_size
    while pos + 8 <= len(data):
        rtype, size = struct.unpack_from("<II", data, pos)
        yield rtype, memoryview(data)[pos + 8:pos + 8 + size]
        pos += 8 + size


class Draw:
    pass


def replay_state(path):
    """Walks the capture and yields (draw_index, kind, args, state snapshot)."""
    rs, ss, tex = {}, {}, {}
    vs = ps = decl = 0
    vsf, psf = {}, {}
    streams, rt = {}, {}
    names, shaders, decls, surfaces = {}, {}, {}, {}
    textures = {}
    n = 0
    for t, p in records(path):
        if t in (1, 2, 3):
            tid, w, h, d, lv, usage, fmt, pool = struct.unpack_from("<Q7I", p)
            textures[tid] = (w, h, fmt, usage)
        elif t == 5:
            sid, tid, face, level = struct.unpack_from("<QQII", p)
            surfaces[sid] = f"tex {names.get(tid, hex(tid))} L{level}"
        elif t == 4:
            sid, w, h, fmt, usage, pool, ms, msq = struct.unpack_from("<Q7I", p)
            surfaces[sid] = f"surf {w}x{h} fmt{fmt}"
        elif t == 6:
            surfaces[struct.unpack_from("<Q", p)[0]] = "backbuffer"
        elif t in (9, 10):
            sid = struct.unpack_from("<Q", p)[0]
            shaders[sid] = bytes(p[8:])
        elif t == 11:
            did = struct.unpack_from("<Q", p)[0]
            els = []
            for i in range(8, len(p) - 7, 8):
                stream, off, typ, meth, usage, uidx = struct.unpack_from("<HHBBBB", p, i)
                if stream == 0xFF:
                    break
                els.append((stream, off, DECLTYPE.get(typ, typ), DECLUSAGE.get(usage, usage), uidx))
            decls[did] = els
        elif t == 132:
            tid = struct.unpack_from("<Q", p)[0]
            names[tid] = bytes(p[8:]).decode(errors="replace")
        elif t == 64:
            s, v = struct.unpack_from("<II", p); rs[s] = v
        elif t == 65:
            st, ty, v = struct.unpack_from("<III", p); ss[(st, ty)] = v
        elif t == 67:
            slot, tid = struct.unpack_from("<IQ", p); tex[slot] = tid
        elif t == 68:
            vs = struct.unpack_from("<Q", p)[0]
        elif t == 69:
            ps = struct.unpack_from("<Q", p)[0]
        elif t == 70:
            decl = struct.unpack_from("<Q", p)[0]
        elif t in (72, 73):
            start, count = struct.unpack_from("<II", p)
            target = vsf if t == 72 else psf
            for i in range(count):
                target[start + i] = struct.unpack_from("<4f", p, 8 + 16 * i)
        elif t == 78:
            stream, sid, off, stride = struct.unpack_from("<IQII", p); streams[stream] = (sid, off, stride)
        elif t == 81:
            slot, sid = struct.unpack_from("<IQ", p); rt[slot] = sid
        elif t in (96, 97, 98, 99):
            d = Draw()
            d.index, d.kind = n, {96: "DP", 97: "DIP", 98: "DPUP", 99: "DIPUP"}[t]
            if t == 96:
                d.prim, d.start, d.count = struct.unpack_from("<III", p)
            elif t == 97:
                d.prim, base, mn, nv, si, d.count = struct.unpack_from("<IiIIII", p)
            elif t == 98:
                d.prim, d.count, stride, vb = struct.unpack_from("<IIII", p)
            else:
                d.prim, mn, nv, d.count, ifmt, stride, ib, vb = struct.unpack_from("<8I", p)
            d.rs, d.ss, d.tex = dict(rs), dict(ss), dict(tex)
            d.vs, d.ps, d.decl = vs, ps, decl
            d.vsf, d.psf = dict(vsf), dict(psf)
            d.rt0 = surfaces.get(rt.get(0, 0), hex(rt.get(0, 0)))
            d.names, d.shaders, d.decls, d.textures = names, shaders, decls, textures
            n += 1
            yield d


# ---- shader names ----

_shader_index = None


def shader_index():
    global _shader_index
    if _shader_index is None:
        _shader_index = {}
        for f in SHADER_DIR.rglob("*.?po"):
            _shader_index[hashlib.sha1(f.read_bytes()).hexdigest()] = f
    return _shader_index


def shader_name(bytecode):
    if not bytecode:
        return "-"
    f = shader_index().get(hashlib.sha1(bytecode).hexdigest())
    return f.stem if f else f"?{hashlib.sha1(bytecode).hexdigest()[:8]}"


def disassemble(bytecode):
    d3dx = ctypes.WinDLL("d3dx9_43")
    buf = ctypes.c_void_p()
    hr = d3dx.D3DXDisassembleShader(ctypes.c_char_p(bytecode), 0, None, ctypes.byref(buf))
    if hr != 0 or not buf:
        return f"<disassembly failed 0x{hr & 0xffffffff:08X}>"
    vtbl = ctypes.cast(ctypes.cast(buf, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtbl[2])
    text = ctypes.string_at(get_ptr(buf), get_size(buf)).decode(errors="replace").rstrip("\0")
    release(buf)
    return text


# ---- formatting ----

def blend_text(rs):
    parts = []
    if rs.get(27):
        parts.append(f"blend {BLEND.get(rs.get(19), rs.get(19))}{BLENDOP.get(rs.get(171, 1), '?')}{BLEND.get(rs.get(20), rs.get(20))}")
        if rs.get(206):
            parts.append(f"a:{BLEND.get(rs.get(207))}{BLENDOP.get(rs.get(209, 1))}{BLEND.get(rs.get(208))}")
    else:
        parts.append("opaque")
    if rs.get(15):
        parts.append(f"atest {CMP.get(rs.get(25), rs.get(25))}{rs.get(24)}")
    parts.append(("z" if rs.get(7) else "noz") + ("w" if rs.get(14) else ""))
    if rs.get(168, 15) != 15:
        parts.append(f"cw{rs.get(168):x}")
    return " ".join(parts)


def tex_text(d, stages=range(8)):
    out = []
    for s in stages:
        tid = d.tex.get(s, 0)
        if tid:
            name = d.names.get(tid)
            if not name:
                w, h, fmt, usage = d.textures.get(tid, (0, 0, 0, 0))
                name = f"{'RT' if usage & 1 else ''}{w}x{h}"
            out.append(f"s{s}:{name}")
    return " ".join(out)


def resolve(path):
    p = Path(path)
    if p.exists():
        return p
    p = CAPTURE_DIR / (path if path.endswith(".ds1cap") else path + ".ds1cap")
    if not p.exists():
        sys.exit(f"capture not found: {path}")
    return p


def cmd_draws(a):
    lo, hi = 0, 1 << 30
    if a.range:
        lo, hi = (int(x) for x in a.range.split("-"))
    for d in replay_state(resolve(a.capture)):
        if not lo <= d.index <= hi:
            continue
        vsn, psn = shader_name(d.shaders.get(d.vs)), shader_name(d.shaders.get(d.ps))
        if a.ps and a.ps.lower() not in psn.lower():
            continue
        if a.vs and a.vs.lower() not in vsn.lower():
            continue
        tt = tex_text(d)
        if a.tex and a.tex.lower() not in tt.lower():
            continue
        if a.blend and not d.rs.get(27):
            continue
        print(f"{d.index:4} {d.kind:5} {d.count:6} | {vsn:32} {psn:42} | {blend_text(d.rs):34} | {tt} | rt0 {d.rt0}")


def cmd_draw(a):
    for d in replay_state(resolve(a.capture)):
        if d.index != a.index:
            continue
        print(f"draw {d.index} {d.kind} prims {d.count} prim type {d.prim}")
        print(f"VS {shader_name(d.shaders.get(d.vs))}   PS {shader_name(d.shaders.get(d.ps))}")
        print(f"state: {blend_text(d.rs)}   cull {d.rs.get(22)}  srgbwrite {d.rs.get(194)}  rt0 {d.rt0}")
        print("render states:", ", ".join(f"{RS_NAMES.get(k, k)}={v}" for k, v in sorted(d.rs.items()) if k in RS_NAMES))
        print("textures:", tex_text(d, range(16)))
        for (st, ty), v in sorted(d.ss.items()):
            if d.tex.get(st) and ty in (1, 2, 5, 6, 7, 11):
                print(f"  sampler s{st} type {ty} = {v}")
        print("decl:", d.decls.get(d.decl))
        print("VS constants (non-zero):")
        for r in sorted(d.vsf):
            v = d.vsf[r]
            if any(abs(x) > 1e-9 for x in v):
                print(f"  c{r:<3} {v[0]: .5g} {v[1]: .5g} {v[2]: .5g} {v[3]: .5g}")
        print("PS constants (non-zero):")
        for r in sorted(d.psf):
            v = d.psf[r]
            if any(abs(x) > 1e-9 for x in v):
                print(f"  c{r:<3} {v[0]: .5g} {v[1]: .5g} {v[2]: .5g} {v[3]: .5g}")
        return
    sys.exit("no such draw")


def decode_vertex(blob, decl):
    out = []
    for stream, off, typ, usage, uidx in decl:
        if stream != 0:
            continue
        if typ == "F1": v = struct.unpack_from("<f", blob, off)
        elif typ == "F2": v = struct.unpack_from("<2f", blob, off)
        elif typ == "F3": v = struct.unpack_from("<3f", blob, off)
        elif typ == "F4": v = struct.unpack_from("<4f", blob, off)
        elif typ == "D3DCOLOR":
            b, g, r, al = struct.unpack_from("<4B", blob, off); v = (r, g, b, al)
        elif typ in ("UB4", "UB4N"): v = struct.unpack_from("<4B", blob, off)
        elif typ == "S2": v = struct.unpack_from("<2h", blob, off)
        elif typ == "S4": v = struct.unpack_from("<4h", blob, off)
        else: v = ("?",)
        fmt = ", ".join(f"{x:.3f}" if isinstance(x, float) else str(x) for x in v)
        out.append(f"{usage}{uidx}=({fmt})")
    return " ".join(out)


def cmd_verts(a):
    """Vertices of a UP draw (DPUP / DIPUP), decoded with its declaration."""
    data = None
    n = -1
    for t, p in records(resolve(a.capture)):
        if t in (96, 97, 98, 99):
            n += 1
            if n == a.index:
                data = (t, bytes(p))
                break
    if not data or data[0] not in (98, 99):
        sys.exit("not a UP draw")
    d = next(x for x in replay_state(resolve(a.capture)) if x.index == a.index)
    decl = d.decls.get(d.decl, [])
    t, p = data
    if t == 98:
        prim, count, stride, vbytes = struct.unpack_from("<4I", p)
        vb = p[16:16 + vbytes]
    else:
        prim, mn, nv, count, ifmt, stride, ib, vbytes = struct.unpack_from("<8I", p)
        vb = p[32 + ib:32 + ib + vbytes]
    for i in range(min(len(vb) // stride, a.max)):
        print(f"v{i:<3} {decode_vertex(vb[i * stride:(i + 1) * stride], decl)}")


def texture_image(capture, name):
    """A captured texture's top level as a PIL image, by TPF name or hex id.
    Returns (image, label, fmt) or None."""
    import io
    from PIL import Image
    want = name.lower()
    names, descs = {}, {}
    for t, p in records(resolve(capture)):
        if t == 132:
            tid = struct.unpack_from("<Q", p)[0]; names[tid] = bytes(p[8:]).decode(errors="replace")
        elif t in (1, 2, 3):
            tid, w, h, d_, lv, usage, fmt, pool = struct.unpack_from("<Q7I", p); descs[tid] = (w, h, fmt)
        elif t == 32:
            tid, flags, face, level, rows, row_bytes, depth = struct.unpack_from("<QIIIIII", p)
            label = names.get(tid, "")
            if level or face or not (want == label.lower() or want == hex(tid)):
                continue
            w, h, fmt = descs[tid]
            pixels = bytes(p[32:32 + rows * row_bytes])
            fourcc = {0x31545844: b"DXT1", 0x33545844: b"DXT3", 0x35545844: b"DXT5"}.get(fmt)
            if fourcc:
                hdr = struct.pack("<4sI6I44x8I5I", b"DDS ", 124, 0x1007 | 0x80000, h, w, len(pixels), 0, 1,
                                  32, 4, struct.unpack("<I", fourcc)[0], 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
                img = Image.open(io.BytesIO(hdr + pixels))
            elif fmt in (21, 22):
                img = Image.frombuffer("RGBA", (w, h), pixels, "raw", "BGRA", row_bytes, 1)
            else:
                sys.exit(f"format {fmt} not supported")
            return img, label or hex(tid), fmt
    return None


def cmd_texture(a):
    """Writes a captured texture's top level to PNG (via a DDS wrapper for DXT formats)."""
    found = texture_image(a.capture, a.name)
    if not found:
        sys.exit("texture not found")
    img, label, fmt = found
    img.save(a.out)
    print(f"{label}: {img.size[0]}x{img.size[1]} fmt {fmt} -> {a.out}")


def cmd_shader(a):
    matches = [f for f in shader_index().values() if a.name.lower() in f.name.lower()]
    if not matches:
        sys.exit("no such shader")
    for f in sorted(matches)[: a.max]:
        print(f"==== {f.relative_to(SHADER_DIR)}")
        print(disassemble(f.read_bytes()))


def cmd_shaders(a):
    use = collections.Counter()
    for d in replay_state(resolve(a.capture)):
        use[(shader_name(d.shaders.get(d.vs)), shader_name(d.shaders.get(d.ps)))] += 1
    for (vsn, psn), n in sorted(use.items(), key=lambda kv: -kv[1]):
        print(f"{n:5}  {vsn:34} {psn}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("draws"); p.add_argument("capture"); p.add_argument("--ps"); p.add_argument("--vs")
    p.add_argument("--tex"); p.add_argument("--blend", action="store_true"); p.add_argument("--range"); p.set_defaults(fn=cmd_draws)
    p = sub.add_parser("draw"); p.add_argument("capture"); p.add_argument("index", type=int); p.set_defaults(fn=cmd_draw)
    p = sub.add_parser("shader"); p.add_argument("capture"); p.add_argument("name"); p.add_argument("--max", type=int, default=4)
    p.set_defaults(fn=cmd_shader)
    p = sub.add_parser("shaders"); p.add_argument("capture"); p.set_defaults(fn=cmd_shaders)
    p = sub.add_parser("verts"); p.add_argument("capture"); p.add_argument("index", type=int); p.add_argument("--max", type=int, default=12)
    p.set_defaults(fn=cmd_verts)
    p = sub.add_parser("texture"); p.add_argument("capture"); p.add_argument("name"); p.add_argument("out"); p.set_defaults(fn=cmd_texture)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
