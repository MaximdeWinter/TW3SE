"""The game's texture.cache: read it, write one, and build an entry from a DDS file.

    python tools\\texcache.py info <texture.cache>                    entries with size, format, pieces
    python tools\\texcache.py dds <texture.cache> <name.xbm> <out.dds>  one texture as DDS (all mips)
    python tools\\texcache.py check <texture.cache>                   rebuild it from its parts, compare the bytes

Layout (versions 6 and 7 seen): the textures' data, each from a 4096-byte page; a table of u32 piece offsets; the
names (0-terminated); 52 bytes per entry; a footer of 32 bytes: FNV-1a 64 over table, names and entries, the pages
used, the entry count, the names' size, the table's count, "HCXT", the version.

An entry: key (the .xbm's texture cache key), name offset, first page, stored size, size, 16, width, height, mips,
slices, first table index, pieces - 1 (version 7: 0x10000 added), timestamp, format (7 DXT1, 8 DXT5, 13 DXT3,
10 BC7, 14 BC4, 15 BC5, 253 RGBA), 4, cube, 0. Its data is one piece per mip (u32 stored size, u32 size, u8 pieces
left, zlib), the largest first; the mips whose longer side is 32 or less share the last piece. The table holds the
offsets of the pieces after the first, from the entry's start.
"""
import struct
import sys
import zlib

ENTRY = struct.Struct('<IiIIIIHHHHiiqBBBB')
FOOTER = struct.Struct('<QIIIIII')
BLOCK = {7: 8, 8: 16, 13: 16, 10: 16, 14: 8, 15: 16}  # bytes per 4x4 block
FOURCC = {7: b'DXT1', 8: b'DXT5', 13: b'DXT3', 14: b'ATI1', 15: b'ATI2'}


def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def mip_size(fmt, w, h):
    if fmt in BLOCK:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK[fmt]
    return w * h * 4


def split(fmt, w, h, mips):
    """The sizes of an entry's pieces: one per mip whose longer side is above 32, the rest together."""
    sizes, tail = [], 0
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m)
        s = mip_size(fmt, mw, mh)
        if mw > 32 or mh > 32:  # (checked against the game's caches: tools\texcache.py splits)
            sizes.append(s)
        else:
            tail += s
    if tail:
        sizes.append(tail)
    return sizes


def zstore(data):
    """zlib with stored blocks (no compression): what any zlib reader takes."""
    out = bytearray(b'\x78\x01')
    for i in range(0, max(len(data), 1), 0xffff):
        part = data[i:i + 0xffff]
        last = i + 0xffff >= len(data)
        out += bytes([1 if last else 0]) + struct.pack('<HH', len(part), 0xffff ^ len(part)) + part
    return bytes(out + struct.pack('>I', zlib.adler32(data)))


class Cache:
    def __init__(self, data):
        self.data = data
        crc, self.pages, count, strsize, mips, ident, self.version = FOOTER.unpack_from(data, len(data) - 32)
        if ident != 0x54584348:
            raise ValueError('not a texture.cache')
        end = len(data) - 32
        eoff = end - count * ENTRY.size
        noff = eoff - strsize
        toff = noff - mips * 4
        self.crc = crc
        self.table = list(struct.unpack_from(f'<{mips}I', data, toff))
        names = data[noff:eoff]
        self.entries = []
        for k in range(count):
            e = list(ENTRY.unpack_from(data, eoff + k * ENTRY.size))
            name = names[e[1]:names.index(b'\0', e[1])].decode('utf-8', 'replace')
            self.entries.append((name, e))

    @staticmethod
    def open(path):
        """The index only (read from the end); pieces() reads a texture's data from the file."""
        f = open(path, 'rb')
        f.seek(-32, 2)
        size = f.tell() + 32
        count, strsize, mips = FOOTER.unpack(f.read(32))[2:5]
        tail = count * ENTRY.size + strsize + mips * 4 + 32
        f.seek(size - tail)
        c = Cache(f.read(tail))
        c.file, c.size = f, size
        return c

    def read(self, offset, n):
        if getattr(self, 'file', None):
            self.file.seek(offset)
            return self.file.read(n)
        return self.data[offset:offset + n]

    def pieces(self, e):
        raw = self.read(e[2] * 4096, e[3])
        out, i = [], 0
        while i < e[3]:
            ps, s, left = struct.unpack_from('<IIB', raw, i)
            out.append((ps, s, left, raw[i + 9:i + 9 + ps]))
            i += 9 + ps
        return out

    def find(self, name):
        name = name.lower()
        for n, e in self.entries:
            if n.lower() == name:
                return e
        return None


def write(textures, version=6):
    """textures: [(name, entry fields [key, _, _, _, _, align, w, h, mips, slices, _, _, ts, fmt, f2, cube, x],
    pieces [(stored size, size, zlib bytes)])] -> the bytes of a texture.cache"""
    body = bytearray()
    table, names, entries = [], bytearray(), bytearray()
    for name, e, pieces in textures:
        while len(body) % 4096:
            body.append(0)
        page = len(body) // 4096
        start = len(body)
        offsets = []
        for k, (ps, s, z) in enumerate(pieces):
            if k:
                offsets.append(len(body) - start)
            body += struct.pack('<IIB', ps, s, len(pieces) - 1 - k) + z
        e = list(e)
        e[1] = len(names)
        e[2] = page
        e[3] = len(body) - start
        e[4] = sum(s for _, s, _ in pieces)
        e[10] = len(table)
        e[11] = len(offsets) + (0x10000 if version >= 7 else 0)
        table += offsets
        names += name.encode() + b'\0'
        entries += ENTRY.pack(*e)
    while len(body) % 4096:
        body.append(0)
    tail = struct.pack(f'<{len(table)}I', *table) + bytes(names) + bytes(entries)
    footer = FOOTER.pack(fnv1a64(tail), len(body) // 4096, len(textures), len(names), len(table), 0x54584348, version)
    return bytes(body) + tail + footer


def read_dds(data):
    """-> (format, width, height, mips, pixel bytes) for DXT1/3/5, BC4, BC5, BC7 (DX10 98) and RGBA"""
    if data[:4] != b'DDS ':
        raise ValueError('not a DDS file')
    h, w = struct.unpack_from('<II', data, 12)
    mips = max(1, struct.unpack_from('<I', data, 28)[0])
    flags, fourcc, bits = struct.unpack_from('<I4sI', data, 80)
    start = 128
    fmt = {v: k for k, v in FOURCC.items()}.get(fourcc)
    if fourcc == b'DX10':
        dxgi = struct.unpack_from('<I', data, 128)[0]
        start += 20
        fmt = {98: 10, 99: 10, 71: 7, 72: 7, 77: 8, 78: 8, 74: 13, 80: 14, 83: 15}.get(dxgi)
    elif not flags & 4 and bits == 32:
        fmt = 253
    if fmt is None:
        raise ValueError(f'a DDS format the game does not cache ({fourcc!r})')
    return fmt, w, h, mips, data[start:]


def entry_from_dds(dds, like):
    """The pieces of a DDS for a texture like the game's entry `like` (key, format and size have to match)."""
    fmt, w, h, mips, pix = read_dds(dds)
    if (fmt, w, h, mips) != (like[13], like[6], like[7], like[8]):
        raise ValueError(f'the DDS is format {fmt} {w}x{h} with {mips} mips, the game\'s texture format {like[13]} '
                         f'{like[6]}x{like[7]} with {like[8]} mips')
    sizes = split(fmt, w, h, mips)
    if sum(sizes) > len(pix):
        raise ValueError('the DDS is shorter than its mips')
    pieces, at = [], 0
    for s in sizes:
        z = zstore(pix[at:at + s])
        pieces.append((len(z), s, z))
        at += s
    return pieces


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ''
    if cmd == 'info':
        c = Cache.open(argv[2])
        word = argv[3].lower() if len(argv) > 3 else ''
        print(f'version {c.version}, {len(c.entries)} entries, {c.pages} pages')
        for name, e in [x for x in c.entries if word in x[0].lower()][:200]:
            print(f'{name}  {e[6]}x{e[7]} mips {e[8]} format {e[13]} key {e[0]:08x} pieces {e[11] & 0xffff}')
    elif cmd == 'dds':
        c = Cache.open(argv[2])
        e = c.find(argv[3])
        if not e:
            sys.exit('no texture ' + argv[3])
        pix = b''.join(zlib.decompress(z) for _, _, _, z in c.pieces(e))
        fmt, w, h, mips = e[13], e[6], e[7], e[8]
        top = mip_size(fmt, w, h)
        fourcc = FOURCC.get(fmt, b'DX10' if fmt == 10 else b'\0\0\0\0')
        pf = struct.pack('<II4sIIIII', 32, 4, fourcc, 0, 0, 0, 0, 0) if fmt != 253 else \
            struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0xff0000, 0xff00, 0xff, 0xff000000)
        head = b'DDS ' + struct.pack('<IIIIIII', 124, 0x1007 | 0x20000 | 0x80000, h, w, top, 0, mips) + b'\0' * 44 + pf + \
            struct.pack('<IIIII', 0x401008, 0, 0, 0, 0)
        if fmt == 10:
            head += struct.pack('<IIIII', 98, 3, 0, 1, 0)
        open(argv[4], 'wb').write(head + pix)
        print(f'{argv[4]}: {w}x{h}, {mips} mips, format {fmt}')
    elif cmd == 'splits':  # big caches: index and piece heads only, by seeking
        for path in argv[2:]:
            with open(path, 'rb') as f:
                f.seek(-32, 2)
                size = f.tell() + 32
                crc, pages, count, strsize, mips, ident, ver = FOOTER.unpack(f.read(32))
                f.seek(size - 32 - count * ENTRY.size - strsize)
                names = f.read(strsize)
                raw = f.read(count * ENTRY.size)
                bad = shapes = layers = 0
                for k in range(count):
                    e = ENTRY.unpack_from(raw, k * ENTRY.size)
                    got, i = [], 0
                    while i < e[3] and len(got) < 64:
                        f.seek(e[2] * 4096 + i)
                        ps, s, _ = struct.unpack('<IIB', f.read(9))
                        got.append(s)
                        i += 9 + ps
                    if e[9] != 1:  # texture arrays and cube maps: not what a DDS replaces
                        layers += 1
                        continue
                    if e[6] != e[7]:
                        shapes += 1
                    if got != split(e[13], e[6], e[7], e[8]) and bad < 8:
                        n = names[e[1]:names.index(b'\0', e[1])].decode('utf-8', 'replace')
                        print('  differs:', n, e[6], e[7], e[8], e[9], e[13], got)
                    bad += got != split(e[13], e[6], e[7], e[8])
                n1 = count - layers
                print(f'{path}: version {ver}, {n1 - bad} of {n1} single textures as split() says ({shapes} not square)')
    elif cmd == 'check':
        c = Cache(open(argv[2], 'rb').read())
        bad = 0
        tex = []
        for name, e in c.entries:
            ps = c.pieces(e)
            if [s for _, s, _, _ in ps] != split(e[13], e[6], e[7], e[8]):
                bad += 1
                if bad <= 10:
                    print('split differs:', name, e[6], e[7], e[8], e[13], [s for _, s, _, _ in ps])
            tex.append((name, e, [(p, s, z) for p, s, _, z in ps]))
        print(f'{len(c.entries) - bad} of {len(c.entries)} entries split as split() says')
        if len(c.data) < 400_000_000:
            same = write(tex, c.version) == c.data
            print('rebuilt byte for byte:', same)


if __name__ == '__main__':
    main(sys.argv)
