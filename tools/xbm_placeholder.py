"""A texture to paint at run time: a .xbm of the group GUIWithAlpha (the game keeps it in memory at its full size, so
TextureWrite can fill it), N x N, made from the game's own GUI texture. Put it under tw3se\\files\\ at a depot path of
your own; TW3SE packs it at the next start. w3.files.alias gives it more paths, each one a texture of its own.

    python tools\\xbm_placeholder.py <game folder> <size> <out.xbm> [r g b a]

The size is a power of two from 4 to 4096. The picture until the first TextureWrite: one colour (grey by default).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cr2w import CR2W, Depot  # noqa: E402

SOURCE = "gameplay\\gui\\inventory\\inventory_background.xbm"  # the game's only GUIWithAlpha texture: 64 x 64 BC3


def bc3_block(r, g, b, a):
    """One BC3 block of a single colour (alpha block, then a DXT1 colour block)."""
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return bytes([a, a, 0, 0, 0, 0, 0, 0]) + struct.pack("<HHI", c, c, 0)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    game, n, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    rgba = [int(v) for v in sys.argv[4:8]] if len(sys.argv) >= 8 else [128, 128, 128, 255]
    if n < 4 or n > 4096 or n & (n - 1):
        sys.exit("the size has to be a power of two from 4 to 4096")
    c = CR2W(Depot(game, mods=False).read(SOURCE))
    chunk = c.exports[0][4]
    props = c.props(chunk)
    for name, _t, off, _size in props:
        if name in ("width", "height"):
            struct.pack_into("<I", chunk, off, n)
    end = props[-1][2] + props[-1][3] + 2  # after the last value and the closing name 0
    z, cnt, w, h, _pitch, size, x = struct.unpack_from("<7I", chunk, end)  # 0, mips, w, h, pitch, bytes, 16
    if (z, cnt, w, h) != (0, 1, 64, 64):
        sys.exit("the game's GUI texture is not as expected (a patch of the game?)")
    blocks = (n // 4) ** 2
    tail = struct.pack("<7I", 0, 1, n, n, n // 4 * 16, blocks * 16, x) + bc3_block(*rgba) * blocks
    c.exports[0][4] = chunk[:end] + tail
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "wb").write(c.save())
    print(f"{out}: {n} x {n}, GUIWithAlpha")


if __name__ == "__main__":
    main()
