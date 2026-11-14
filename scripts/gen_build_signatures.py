"""Generate the address table in src/comp/game/game_build.cpp.

Every fixed game address the proxy uses is listed below by its 2015-06-15 VA. For each one this
finds where it lives in the 2015-01-14 build and builds a fallback lookup for any other build:

  .text   signature anchored at the address (call targets and absolute operands wildcarded)
  .data   signature of an instruction that embeds the address; the operand is read back
  .rdata  inside a vtable: the vtable found through its RTTI class name, plus the slot offset

A signature is kept only if it matches exactly once in .text of BOTH builds and resolves to
the right address in each. The 2015-01-14 address is cross-checked by a second, independent
method (other references to the same data must agree; code gets a second signature window).

    python -I gen_build_signatures.py <2015-06-15 exe> <2015-01-14 exe> [out.inc]
"""
import re, struct, sys, collections

LO, HI = 0x400000, 0x1600000

# (current VA, name used in the logs, where in the proxy)
ADDRS = [
    (0x013783C0, 'hgman_slot', 'render_toggles, camera, point_lights'),
    (0x01378464, 'drawenvman_slot', 'render_toggles'),
    (0x01378520, 'field_area_active', 'map_lights'),
    (0x0137D938, 'point_light_man', 'point_lights'),
    (0x012DFF34, 'shadowmap_size', 'render_toggles'),
    (0x012DEFEB, 'group_map', 'render_toggles'),
    (0x012DF242, 'group_chr', 'render_toggles'),
    (0x012DF241, 'group_obj', 'render_toggles'),
    (0x012DF243, 'group_sfx', 'render_toggles'),
    (0x012DF3D3, 'group_lightshaft', 'render_toggles'),
    (0x012DF3D1, 'group_remo', 'render_toggles'),
    (0x01378490, 'remo_man', 'cutscenes'),
    (0x0137D790, 'event_man', 'cutscenes'),
    (0x0137D644, 'world_chr_man', 'cutscenes'),
    (0x01378564, 'frpg_fx', 'ffx_instances'),
    (0x01378700, 'game_data_man', 'estus'),
    (0x01378A14, 'window_list', 'aspect_ratio'),
    (0x0137D99C, 'draw_param_man', 'map_sun'),
    (0x0137E7A0, 'fx_host_offset', 'fx_owner'),
    (0x012E29E8, 'waterfall_anim_base', 'waterfall'),
    (0x011E7E90, 'frame_step', 'fps_unlock'),

    (0x01174478, 'vt_frpg_camera_entity', 'camera'),
    (0x011AB110, 'vt_bank_point_light', 'point_lights'),
    (0x011AB24C, 'vt_direct_point_light', 'point_lights'),
    (0x011AB394, 'vt_lerp_bank_point_light', 'point_lights'),
    (0x011309CC, 'vt_ffx_root', 'ffx_instances'),
    (0x011A44F4, 'vt_ffx_adapter', 'ffx_instances'),
    (0x011A5254, 'vt_point_light_host', 'fx_owner'),
    (0x011D0404, 'vt_menu_dc', 'hud_scale'),
    (0x011DCDAC, 'slot_script_update', 'cutscenes'),
    (0x011E7318, 'slot_fx_draw_0', 'fx_draw_owner'),
    (0x011E7328, 'slot_fx_draw_1', 'fx_draw_owner'),
    (0x011E7338, 'slot_fx_draw_2', 'fx_draw_owner'),
    (0x011E7348, 'slot_fx_draw_3', 'fx_draw_owner'),
    (0x011E735C, 'slot_fx_draw_4', 'fx_draw_owner'),
    (0x011E7370, 'slot_fx_draw_5', 'fx_draw_owner'),
    (0x011E7384, 'slot_fx_draw_6', 'fx_draw_owner'),
    (0x011E7398, 'slot_fx_draw_7', 'fx_draw_owner'),
    (0x011E73AC, 'slot_fx_draw_8', 'fx_draw_owner'),

    (0x00467D70, 'set_enable_emitter', 'render_toggles'),
    (0x00467D00, 'stage_mask_emitter', 'render_toggles'),
    (0x00EAF61D, 'water_reflect_je', 'render_toggles'),
    (0x00F8F053, 'envmap_je', 'render_toggles'),
    (0x00593E40, 'frustum_region_aabb', 'frustum_culling'),
    (0x005C5720, 'frustum_swept_segment', 'frustum_culling'),
    (0x005C5870, 'frustum_swept_parallelogram', 'frustum_culling'),
    (0x005AEF50, 'frustum_oriented_box', 'frustum_culling'),
    (0x0041BA00, 'window_list_add', 'aspect_ratio'),
    (0x00453720, 'projection', 'aspect_ratio'),
    (0x00F7016C, 'table_last_width', 'aspect_ratio'),
    (0x00F70177, 'table_last_height', 'aspect_ratio'),
    (0x0054E150, 'fx_draw_0', 'fx_draw_owner'),
    (0x0054E820, 'fx_draw_1', 'fx_draw_owner'),
    (0x0054F060, 'fx_draw_2', 'fx_draw_owner'),
    (0x005508A0, 'fx_draw_3', 'fx_draw_owner'),
    (0x00551560, 'fx_draw_4', 'fx_draw_owner'),
    (0x00551BB0, 'fx_draw_5', 'fx_draw_owner'),
    (0x005523C0, 'fx_draw_6', 'fx_draw_owner'),
    (0x00552930, 'fx_draw_7', 'fx_draw_owner'),
    (0x00553AC0, 'fx_draw_8', 'fx_draw_owner'),
    (0x006F0F40, 'fx_draw_slot_after', 'fx_draw_owner'),
    (0x00787E90, 'dialog_draw', 'hud_scale'),
    (0x00788D80, 'object_draw', 'hud_scale'),
    (0x00BBF3B0, 'script_update', 'cutscenes'),
    (0x00D09DA0, 'nearest_sampler', 'cutscenes'),
    (0x00D0A1F0, 'hermite_sampler', 'cutscenes'),
    (0x00EE1950, 'remo_request', 'cutscenes'),
    (0x00FC2084, 'camera_call', 'cutscenes'),
    (0x00EE5919, 'set_logo_flag', 'intro_skip'),
    (0x00BAC4ED, 'draw_command_call', 'fps_unlock'),
    (0x00FFB688, 'interval_store', 'fps_unlock'),
]


class Img:
    def __init__(self, p):
        d = open(p, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        self.ts = struct.unpack_from('<I', d, pe + 8)[0]
        self.base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
        self.imgsz = struct.unpack_from('<I', d, pe + 24 + 56)[0]
        m = bytearray(self.imgsz)
        hdrsz = struct.unpack_from('<I', d, pe + 24 + 60)[0]
        m[:hdrsz] = d[:hdrsz]
        self.secs = {}
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            name, vsz, va, rsz, rptr = struct.unpack_from('<8sIIII', d, o)
            name = name.rstrip(b'\0').decode()
            n = min(rsz, vsz)
            m[va:va + n] = d[rptr:rptr + n]
            self.secs[name] = (self.base + va, vsz)
        self.m = bytes(m)
        self._text = self.sec('.text')

    def at(self, va, n):
        return self.m[va - self.base:va - self.base + n]

    def dword(self, va):
        return struct.unpack_from('<I', self.m, va - self.base)[0]

    def sec(self, name):
        s, z = self.secs[name]
        return s, self.at(s, z)

    def sec_of(self, va):
        for k, (s, z) in self.secs.items():
            if s <= va < s + z:
                return k


def wildcard_mask(b):
    wild = [False] * len(b)
    for i in range(len(b)):
        if b[i] in (0xE8, 0xE9) and i + 5 <= len(b):
            for k in range(1, 5): wild[i + k] = True
        if b[i] == 0x0F and i + 6 <= len(b) and 0x80 <= b[i + 1] <= 0x8F:
            for k in range(2, 6): wild[i + k] = True
    for i in range(len(b) - 3):
        if LO <= struct.unpack_from('<I', b, i)[0] < HI:
            for k in range(4): wild[i + k] = True
    return wild


def to_regex(b, wild):
    return re.compile(b''.join(b'.' if w else re.escape(bytes([c])) for c, w in zip(b, wild)), re.S)


def to_text(b, wild):
    return ' '.join('??' if w else '%02X' % c for c, w in zip(b, wild))


def scan(img, rx, limit=2):
    s, t = img._text
    out = []
    for m in rx.finditer(t):
        out.append(m.start() + s)
        if len(out) >= limit:
            break
    return out


BEFORES = (0, 1, 2, 3, 4, 6, 8, 12, 16, 24)
AFTERS = (12, 16, 20, 24, 32, 40, 48, 56, 64, 80, 96, 128, 160, 192, 256)


def make_sig(new, old, anchor_new, resolve_old, min_concrete=14, skip=None):
    """Shortest signature anchored at anchor_new (start + off == anchor) that is unique in .text of
    both builds. resolve_old(anchor_in_old) -> value in old (or None). Returns (len, text, off, old_value)."""
    best = None
    for before in BEFORES:
        for after in AFTERS:
            b = bytearray(new.at(anchor_new - before, before + after))
            wild = wildcard_mask(b)
            if wild[0] or wild.count(False) < min_concrete:
                continue
            txt = to_text(b, wild)
            if skip and txt == skip:
                continue
            rx = to_regex(b, wild)
            hn = scan(new, rx)
            if len(hn) != 1 or hn[0] + before != anchor_new:
                continue
            ho = scan(old, rx)
            if len(ho) == 1:
                v = resolve_old(ho[0] + before)
                if v is not None:
                    cand = (before + after, txt, before, v)
                    if best is None or cand[0] < best[0]:
                        best = cand
            break
        if best and best[0] <= 24:
            break
    return best


# ---- RTTI ------------------------------------------------------------------------------

def col_name(img, col):
    """(class name, COL offset) of a CompleteObjectLocator, or None if col is not one."""
    rs, rz = img.secs['.rdata']
    if not (rs <= col < rs + rz - 20):
        return None
    sig, off, cd, td, chd = struct.unpack_from('<IIIII', img.m, col - img.base)
    if sig != 0 or not (LO <= td < HI):
        return None
    name = img.at(td + 8, 256).split(b'\0')[0]
    if not name.startswith(b'.?AV') and not name.startswith(b'.?AU'):
        return None
    return name.decode(), off


def containing_vtable(img, va, max_back=0x400):
    """(vtable start, class name, COL offset) of the vtable that holds va: walk back to the
    first slot preceded by a COL pointer."""
    p = va
    while va - p <= max_back:
        c = col_name(img, img.dword(p - 4))
        if c:
            return p, c[0], c[1]
        p -= 4
    return None


def rtti_vtables(img, mangled, col_off):
    name = mangled.encode() + b'\0'
    tds = []
    for sec in ('.data', '.rdata'):
        s, t = img.sec(sec)
        tds += [m.start() + s - 8 for m in re.finditer(re.escape(name), t)]
    s, t = img.sec('.rdata')
    out = []
    for td in tds:
        for m in re.finditer(re.escape(struct.pack('<I', td)), t):
            if m.start() % 4: continue
            col = m.start() + s - 12
            sig, off = struct.unpack_from('<II', img.m, col - img.base)
            if sig != 0 or off != col_off: continue
            for v in re.finditer(re.escape(struct.pack('<I', col)), t):
                if v.start() % 4: continue
                out.append(v.start() + s + 4)
    return out


# ---- per kind -------------------------------------------------------------------------

def disjoint_sig(new, old, va, start1, len1, old_va, min_concrete=14):
    """A second signature that shares no byte with the first ([start1, start1 + len1)): a window
    after it or before it, unique in both builds, that puts the address at the same place in each.
    Returns (text, off) with address = match + off (off may be negative)."""
    cands = []
    for gap in (0, 4, 8, 16, 24, 32, 48):
        for length in (16, 20, 24, 32, 40, 48, 64):
            cands.append(start1 + len1 + gap)                # after the first window
            cands.append(start1 - gap - length)              # before it
            for s2 in cands[-2:]:
                b = bytearray(new.at(s2, length))
                wild = wildcard_mask(b)
                if wild[0] or wild.count(False) < min_concrete:
                    continue
                rx = to_regex(b, wild)
                if scan(new, rx) != [s2]:
                    continue
                ho = scan(old, rx)
                if len(ho) == 1 and ho[0] + (va - s2) == old_va:
                    return to_text(b, wild), va - s2
    return None


def gen_code(new, old, va):
    r = make_sig(new, old, va, lambda a: a)
    if not r:
        return None
    # a second window over other bytes must land on the same address in both builds
    r2 = disjoint_sig(new, old, va, va - r[2], r[0], r[3])
    check = 'disjoint second window agrees' if r2 else 'NO DISJOINT SECOND WINDOW'
    return dict(method='code', sig=r[1], off=r[2], adj=0, old=r[3], check=check, ok=True,
                sig2=r2[0] if r2 else '', off2=r2[1] if r2 else 0, refs=0)


def count_refs(img, value):
    s, t = img._text
    return len(re.findall(re.escape(struct.pack('<I', value & 0xFFFFFFFF)), t))


def gen_data(new, old, va):
    s, t = new._text
    refs, adj = [], 0
    # nearest referenced address, looking backwards first (a slot is referenced through the
    # start of the table that holds it)
    for adj in [0] + [d for k in range(1, 0x41) for d in (-k, k)]:
        needle = struct.pack('<I', (va + adj) & 0xFFFFFFFF)
        refs = [m.start() + s for m in re.finditer(re.escape(needle), t)]
        if refs:
            break
    if not refs:
        return None
    votes = collections.Counter()
    sigs = []
    for ref in refs[:16]:
        r = make_sig(new, old, ref, lambda a: (old.dword(a) - adj) & 0xFFFFFFFF)
        if r:
            votes[r[3]] += 1
            sigs.append(r)
    if not sigs:
        return None
    sigs.sort(key=lambda r: r[0])
    best = sigs[0]
    # a second signature, from another reference, that must land on the same address
    second = next((r for r in sigs[1:] if r[3] == best[3]), None)
    agree = votes[best[3]]
    check = '%d/%d references agree' % (agree, sum(votes.values()))
    if adj:
        check += ', referenced as %+d' % adj
    if len(votes) > 1:
        check += ' (others: %s)' % ', '.join('%08X' % k for k in votes if k != best[3])
    # how often code embeds it, in the build that has it least: a lookup must find about as many
    n_refs = min(count_refs(new, va + adj), count_refs(old, best[3] + adj))
    return dict(method='data', sig=best[1], off=best[2], adj=adj, old=best[3], check=check,
                ok=agree * 2 > sum(votes.values()),
                sig2=second[1] if second else '', off2=second[2] if second else 0, refs=n_refs)


def gen_rdata(new, old, va):
    v = containing_vtable(new, va)
    if not v:
        return gen_data(new, old, va)  # no RTTI: through the code that references it
    start, name, col_off = v
    slot = va - start
    got_new = rtti_vtables(new, name, col_off)
    got_old = rtti_vtables(old, name, col_off)
    if got_new != [start] or len(got_old) != 1:
        return dict(method='rtti', sig=name, off=slot, adj=col_off, old=0, ok=False,
                    check='rtti new=%s old=%s' % (got_new, got_old))
    return dict(method='rtti', sig=name, off=slot, adj=col_off, old=got_old[0] + slot, ok=True,
                check='RTTI col+%d slot+0x%X' % (col_off, slot))


def call_target(img, va):
    if img.at(va, 1) != b'\xE8':
        return None
    return (va + 5 + struct.unpack('<i', img.at(va + 1, 4))[0]) & 0xFFFFFFFF


def gen_call(new, old, va, target_new, target_old):
    """A call site in code that exists more than once except for its call target: signature
    anchored at the call, keeping the one match whose call goes to the resolved target."""
    if call_target(new, va) != target_new:
        return None
    for before in (0, 4, 8, 12, 16, 24, 36):
        for after in (16, 24, 32):
            b = bytearray(new.at(va - before, before + after))
            wild = wildcard_mask(b)
            if wild[0] or wild.count(False) < 14:
                continue
            rx = to_regex(b, wild)
            hn = [h + before for h in scan(new, rx, 8) if call_target(new, h + before) == target_new]
            ho = [h + before for h in scan(old, rx, 8) if call_target(old, h + before) == target_old]
            if hn == [va] and len(ho) == 1:
                return dict(method='call', sig=to_text(b, wild), off=before, adj=0, old=ho[0], ok=True,
                            check='the one match calling the resolved target')
    return None


def gen_pointer_at(new, old, va, from_new, from_old, off):
    """An address no signature can single out (a stub with dozens of copies), stored in a
    table entry next to another resolved one."""
    if new.dword(from_new + off) != va:
        return None
    o = old.dword(from_old + off)
    same = old.at(o, 8) == new.at(va, 8)
    return dict(method='pointer', sig='', off=off, adj=0, old=o, ok=same,
                check='dword at +%d of the entry it follows; same bytes in both builds: %s' % (off, same))


def gen_offset(new, old, va, from_new, from_old, delta):
    """A neighbour of another resolved entry, at a fixed distance. The old address is also found
    on its own (code references), and both ways must agree."""
    if from_new + delta != va:
        return None
    own = {'.data': gen_data, '.rdata': gen_rdata}[new.sec_of(va)](new, old, va)
    o = from_old + delta
    ok = bool(own and own['ok'] and own['old'] == o)
    # how often code embeds it, checked at run time like a data entry's
    n_refs = min(count_refs(new, va), count_refs(old, o))
    return dict(method='offset', sig='', off=delta, adj=0, old=o, ok=ok, refs=n_refs,
                check='%+d from the entry it follows; its own lookup agrees: %s' % (delta, ok))


# entries resolved through another one: current VA -> (kind, other entry's current VA, offset).
# The other entry must come earlier in ADDRS.
DERIVED = {
    0x006F0F40: ('pointer', 0x011E7318, 4),     # fx_draw_slot_after = slot_fx_draw_0[1]
    0x00FC2084: ('call', 0x00D09DA0, 0),        # camera_call = the call to nearest_sampler
    # data with a single usable reference: derived from a neighbour that has two signatures
    0x012DF241: ('offset', 0x012DF242, -1),     # group_obj = group_chr - 1
    0x012DF243: ('offset', 0x012DF242, 1),      # group_sfx = group_chr + 1
    0x012DF3D1: ('offset', 0x012DF3D3, -2),     # group_remo = group_lightshaft - 2
    0x011E7384: ('offset', 0x011E7370, 0x14),   # slot_fx_draw_6 = slot_fx_draw_5 + 0x14
    0x011E7398: ('offset', 0x011E7370, 0x28),   # slot_fx_draw_7
    0x011E73AC: ('offset', 0x011E7370, 0x3C),   # slot_fx_draw_8
}


def main():
    new, old = Img(sys.argv[1]), Img(sys.argv[2])
    assert new.ts == 0x557F2FD0 and old.ts == 0x54B6EFC0, (hex(new.ts), hex(old.ts))
    rows, bad = [], 0
    old_of = {}
    for va, name, where in ADDRS:
        sec = new.sec_of(va)
        if va in DERIVED:
            kind, frm, off = DERIVED[va]
            if frm not in old_of:
                r = None
            elif kind == 'pointer':
                r = gen_pointer_at(new, old, va, frm, old_of[frm], off)
            elif kind == 'offset':
                r = gen_offset(new, old, va, frm, old_of[frm], off)
            else:
                r = gen_call(new, old, va, frm, old_of[frm])
            if r:
                r['from'] = frm
        else:
            r = {'.text': gen_code, '.data': gen_data, '.rdata': gen_rdata}[sec](new, old, va)
            if r:
                r['from'] = 0
        if r and r['ok']:
            old_of[va] = r['old']
        if not r or not r['ok'] or old.sec_of(r['old']) != sec:
            bad += 1
            print('FAIL %-28s %08X %s %s' % (name, va, sec, r and (r['check'], '%08X' % r['old'])))
            continue
        rows.append((va, name, where, r))
        print('ok   %-28s %08X -> %08X (%+6d)  %-4s %s' % (name, va, r['old'], r['old'] - va, r['method'], r['check']))

    lines = []
    for va, name, where, r in rows:
        m = {'code': 'how::code', 'data': 'how::data_operand', 'rtti': 'how::rtti_vtable',
             'call': 'how::call_to', 'pointer': 'how::pointer_at', 'offset': 'how::offset_from'}[r['method']]
        check = r['check'] + ('; 2nd signature' if r.get('sig2') else '') + ('; %d refs' % r['refs'] if r.get('refs') else '')
        lines.append('\t\t\t{ 0x%08X, 0x%08X, %s, %d, %d, 0x%08X, %d, %d, "%s", // %s; %s\n\t\t\t\t"%s",\n\t\t\t\t"%s" },' % (
            va, r['old'], m, r['off'], r['adj'], r['from'], r.get('off2', 0), r.get('refs', 0), name, where, check,
            r['sig'], r.get('sig2', '')))
    if len(sys.argv) > 3 and not bad:
        out = sys.argv[3]
        body = '\n'.join(lines) + '\n'
        if out.endswith('.cpp'):
            # replace the rows between the markers in game_build.cpp, keeping its line endings
            src = open(out, 'rb').read().decode('utf-8')
            eol = '\r\n' if '\r\n' in src else '\n'
            begin, end = '// BEGIN GENERATED TABLE', '// END GENERATED TABLE'
            i, j = src.index(begin), src.index(end)
            i = src.index('\n', i) + 1
            j = src.rindex('\n', 0, j) + 1
            src = src[:i] + body.replace('\n', eol) + src[j:]
            open(out, 'wb').write(src.encode('utf-8'))
        else:
            open(out, 'w', newline='\n').write(body)
        print('wrote', out)
    print('%d addresses, %d failed' % (len(ADDRS), bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
