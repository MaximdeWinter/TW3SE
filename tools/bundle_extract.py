r"""Copies game files out of the game's bundles by their depot path, for reading and as a start for your own (an
item XML, a template, a texture's .xbm). A command line over tools\cr2w's Depot: the game, its expansions and DLCs
(--mods: the mods in Mods too), a later bundle over an earlier one, as in the game.

    python tools/bundle_extract.py "gameplay\items\*potion*.xml"                -> .\extracted\gameplay\items\...
    python tools/bundle_extract.py "characters\npc_entities\monsters\wolf*.w2ent" --out D:\wolves
    python tools/bundle_extract.py "gameplay\items\*" --list                    only the paths and sizes

The game folder comes from --game, TW3SE_GAME or Steam's default folder. A pattern takes * and ?, any case. lz4hc
files need `pip install lz4`.
"""
import fnmatch
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cr2w.bundles import Depot  # noqa: E402


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    game = arg("--game", os.environ.get("TW3SE_GAME", r"C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3"))
    pats = [a for a in sys.argv[1:] if not a.startswith("--") and a not in (arg("--game"), arg("--out"))]
    if not pats:
        print(__doc__)
        return 1
    pats = [p.replace("/", "\\").lower() for p in pats]
    depot = Depot(game, mods="--mods" in sys.argv)
    found = sorted(p for p in depot.where if any(fnmatch.fnmatchcase(p, q) for q in pats))
    if not found:
        print("no file fits", " ".join(pats))
        return 1
    if "--list" in sys.argv:
        for p in found:
            b, e = depot.where[p]
            print(f"{e[1]:10d}  {e[0]}   ({os.path.relpath(b, game)})")
        return 0
    out = arg("--out", os.path.join(os.getcwd(), "extracted"))
    done, failed = 0, 0
    for p in found:
        try:
            data = depot.read(p)
        except Exception as e:  # noqa: BLE001 - one file that cannot be read stays a line, the rest go on
            print(f"left out {p}: {e}")
            failed += 1
            continue
        dst = os.path.join(out, depot.where[p][1][0])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        done += 1
    print(f"{done} file(s) to {out}" + (f", {failed} left out" if failed else ""))
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
