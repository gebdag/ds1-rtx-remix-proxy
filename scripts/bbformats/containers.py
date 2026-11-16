"""DCX compression and BND4 / BXF4 binders, as used by Bloodborne."""
import struct
import zlib
from dataclasses import dataclass


# ---------------------------------------------------------------- strings

def utf16z(b, off):
    """Null-terminated UTF-16LE string at off."""
    end = off
    while True:
        end = b.find(b'\0\0', end)
        if end < 0:
            raise ValueError(f'unterminated UTF-16 string at {off:#x}')
        if (end - off) % 2 == 0:
            return bytes(b[off:end]).decode('utf-16-le')
        end += 1


def sjisz(b, off):
    end = b.index(b'\0', off)
    return bytes(b[off:end]).decode('shift_jis', errors='replace')


# ---------------------------------------------------------------- DCX

def is_dcx(b):
    return b[:4] == b'DCX\0'


def dcx_decompress(b):
    """Returns the payload of a DCX file, or b unchanged if it is not DCX."""
    if b[:4] != b'DCX\0':
        return b
    fmt = bytes(b[0x28:0x2C])
    if fmt != b'DFLT':
        raise NotImplementedError(f'DCX format {fmt!r}')
    usize, csize = struct.unpack_from('>II', b, 0x1C)
    dca = b.find(b'DCA\0', 0x20, 0x80)
    start = dca + struct.unpack_from('>I', b, dca + 4)[0]
    out = zlib.decompress(b[start:start + csize])
    if len(out) != usize:
        raise ValueError(f'DCX size mismatch {len(out)} != {usize}')
    return out


def read_file(path):
    with open(path, 'rb') as f:
        return dcx_decompress(f.read())


# ---------------------------------------------------------------- binders

# Binder format flags after bit normalisation (SoulsFormats Binder.Format).
FMT_BIG_ENDIAN = 0x01
FMT_IDS = 0x02
FMT_NAMES1 = 0x04
FMT_NAMES2 = 0x08
FMT_LONG_OFFSETS = 0x10
FMT_COMPRESSION = 0x20

FILE_COMPRESSED = 0x01


def _rev8(x):
    return int(f'{x:08b}'[::-1], 2)


def _normalise_flags(raw, bit_big_endian):
    keep = bit_big_endian or ((raw & 1) and not (raw & 0x80))
    return raw if keep else _rev8(raw)


@dataclass
class BinderFile:
    id: int
    name: str
    flags: int
    offset: int
    size: int
    usize: int
    _src: object = None  # bytes for BND4, bdt path for BXF4

    def read(self, fh=None):
        if isinstance(self._src, (bytes, bytearray, memoryview)):
            raw = bytes(self._src[self.offset:self.offset + self.size])
        elif fh is not None:
            fh.seek(self.offset)
            raw = fh.read(self.size)
        else:
            with open(self._src, 'rb') as f:
                f.seek(self.offset)
                raw = f.read(self.size)
        if raw[:4] == b'DCX\0':
            return dcx_decompress(raw)
        if self.flags & FILE_COMPRESSED:
            return zlib.decompress(raw)
        return raw


def _parse_binder_header(b, magic, src):
    if b[:4] != magic:
        raise ValueError(f'expected {magic!r}, got {bytes(b[:4])!r}')
    e = '>' if b[0x9] else '<'
    bit_be = b[0xA] == 0
    count, = struct.unpack_from(e + 'i', b, 0xC)
    hdr_size, = struct.unpack_from(e + 'q', b, 0x20)
    unicode_ = b[0x30] != 0
    fmt = _normalise_flags(b[0x31], bit_be)

    files = []
    pos = 0x40
    for _ in range(count):
        flags = _normalise_flags(b[pos], bit_be)
        p = pos + 8
        csize, = struct.unpack_from(e + 'q', b, p); p += 8
        usize = csize
        if fmt & FMT_COMPRESSION:
            usize, = struct.unpack_from(e + 'q', b, p); p += 8
        if fmt & FMT_LONG_OFFSETS:
            off, = struct.unpack_from(e + 'Q', b, p); p += 8
        else:
            off, = struct.unpack_from(e + 'I', b, p); p += 4
        fid = -1
        if fmt & FMT_IDS:
            fid, = struct.unpack_from(e + 'i', b, p); p += 4
        name = None
        if fmt & (FMT_NAMES1 | FMT_NAMES2):
            noff, = struct.unpack_from(e + 'I', b, p); p += 4
            name = utf16z(b, noff) if unicode_ else sjisz(b, noff)
        if p - pos != hdr_size:
            raise ValueError(f'file header size {p - pos:#x} != {hdr_size:#x} (fmt {fmt:#x})')
        files.append(BinderFile(fid, name, flags, off, csize, usize, src))
        pos += hdr_size
    return files


def read_bnd4(data):
    """BND4 (optionally DCX-wrapped) -> list[BinderFile]."""
    data = dcx_decompress(data)
    return _parse_binder_header(data, b'BND4', data)


def read_bxf4(bhd_path, bdt_path):
    """Split binder: header file (.xxxbhd) + data file (.xxxbdt)."""
    with open(bhd_path, 'rb') as f:
        bhd = dcx_decompress(f.read())
    return _parse_binder_header(bhd, b'BHF4', bdt_path)
