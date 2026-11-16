"""Packs a mod of the game's Mods folder as a zip for Nexus Mods, with Mods\\<modName>\\ at the top of the zip (players
unpack it into the game folder): its content\\ and tw3se\\ folders and whatever else it has. Left out: .luarc.json and
the editor's own files, __pycache__, *.bak, and the options page TW3SE writes for it (it comes again at each start).

    python tools/pack_mod.py modBetterWolves                       the game from TW3SE_GAME or the default folder
    python tools/pack_mod.py modBetterWolves --game "<game>"       -> modBetterWolves.zip in the current folder
    python tools/pack_mod.py modBetterWolves --out D:\\upload
"""
import os
import sys
import zipfile

SKIP_FILES = {".luarc.json", ".editorconfig", "thumbs.db", "desktop.ini"}
SKIP_DIRS = {"__pycache__", ".git", ".vscode", ".idea"}


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--") and a not in (arg("--game"), arg("--out"))]
    if not names:
        print(__doc__)
        return 1
    mod = names[0]
    game = arg("--game", os.environ.get("TW3SE_GAME", r"C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3"))
    src = os.path.join(game, "Mods", mod)
    if not os.path.isdir(src):
        sys.exit(f"no folder {src}")
    out = os.path.join(arg("--out", os.getcwd()), mod + ".zip")
    count, size = 0, 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(src):
            dirs[:] = [d for d in dirs if d.lower() not in SKIP_DIRS]
            for f in files:
                if f.lower() in SKIP_FILES or f.lower().endswith((".bak", ".tmp", ".pyc")):
                    continue
                p = os.path.join(root, f)
                z.write(p, os.path.join("Mods", mod, os.path.relpath(p, src)))
                count += 1
                size += os.path.getsize(p)
    if not count:
        os.remove(out)
        sys.exit(f"{src} has no files")
    lua = os.path.isfile(os.path.join(src, "tw3se", "init.lua"))
    print(f"{out}: {count} file(s), {size / 1e3:.0f} KB" + (" (a Lua mod: name TW3SE as a requirement)" if lua else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
