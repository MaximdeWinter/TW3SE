"""The game's soundspc.cache (Wwise banks .bnk and streamed sounds .wem): read it, write one.

    python tools\\soundcache.py info <soundspc.cache> [word]      files with offset and size
    python tools\\soundcache.py get <soundspc.cache> <name> <out>   one file out of it
    python tools\\soundcache.py check <soundspc.cache>             rebuild header, names and entries, compare

Layout (version 2): a header of 0x40 bytes ("CS3W", u32 2, two u32 of unknown use, u64 entries offset, u32 count,
u32 0, u64 names offset, u32 names size, u32 of unknown use, u64 largest file rounded up to 4096, FNV-1a 64 over
names and entries), the files back to back from 0x40, the names (0-terminated), 24 bytes per file (u32 name offset,
u32 0, u64 offset, u64 size). Files and names in the order of the entries.
"""
import struct
import sys

HEAD = struct.Struct('<4sIIIQIIQIIQQ')


def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


class Cache:
    def __init__(self, path):
        self.f = open(path, 'rb')
        (magic, self.version, self.u1, self.u2, self.info, count, _, self.names_off, names_size, self.u3,
         self.largest, self.crc) = HEAD.unpack(self.f.read(0x40))
        if magic != b'CS3W' or self.version != 2:
            raise ValueError('not a soundspc.cache of version 2')
        self.f.seek(self.names_off)
        self.names = self.f.read(names_size)
        self.f.seek(self.info)
        raw = self.f.read(count * 24)
        self.entries = []
        for i in range(count):
            no, _, off, size = struct.unpack_from('<IIQQ', raw, i * 24)
            self.entries.append((self.names[no:self.names.index(b'\0', no)].decode('utf-8', 'replace'), off, size))

    def read(self, name):
        for n, off, size in self.entries:
            if n.lower() == name.lower():
                self.f.seek(off)
                return self.f.read(size)
        return None


def layout(files):
    """files: [(name, size)] in their order -> (header without the unknown fields, names, entries, data offsets)"""
    names, entries, offsets = bytearray(), bytearray(), []
    at = 0x40
    for name, size in files:
        offsets.append(at)
        entries += struct.pack('<IIQQ', len(names), 0, at, size)
        names += name.encode() + b'\0'
        at += size
    largest = (max((s for _, s in files), default=0) + 4095) & ~4095
    head = HEAD.pack(b'CS3W', 2, 0, 0, at + len(names), len(files), 0, at, len(names), 0, largest,
                     fnv1a64(bytes(names) + bytes(entries)))
    return head, bytes(names), bytes(entries), offsets


def write(files):
    """files: [(name, bytes)] -> the bytes of a soundspc.cache"""
    head, names, entries, _ = layout([(n, len(b)) for n, b in files])
    return head + b''.join(b for _, b in files) + names + entries


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ''
    c = Cache(argv[2]) if len(argv) > 2 else None
    if cmd == 'info':
        word = argv[3].lower() if len(argv) > 3 else ''
        print(f'{len(c.entries)} files')
        for n, off, size in c.entries:
            if word in n.lower():
                print(f'{n}  {size} bytes at {off:#x}')
    elif cmd == 'get':
        data = c.read(argv[3])
        if data is None:
            sys.exit('no file ' + argv[3])
        open(argv[4], 'wb').write(data)
        print(argv[4], len(data), 'bytes')
    elif cmd == 'check':
        head, names, entries, offsets = layout([(n, s) for n, _, s in c.entries])
        c.f.seek(0)
        real = c.f.read(0x40)
        known = lambda h: h[:8] + h[16:0x2c] + h[0x30:]  # without u1, u2 and u3
        print('entries in data order:', [o for _, o, _ in c.entries] == offsets)
        print('names as rebuilt:', names == c.names)
        c.f.seek(c.info)
        print('entries as rebuilt:', entries == c.f.read(len(entries)))
        print('header as rebuilt (without the unknown fields):', known(head) == known(real))


if __name__ == '__main__':
    main(sys.argv)
