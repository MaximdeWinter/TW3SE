"""The two files for Nexus Mods, from build\\release (package.bat first):

    TW3SE-<version>.zip       for players: unpacked into the game folder (by hand or by a mod manager), it puts
                              bin\\x64_dx12\\dinput8.dll, bin\\x64_dx12\\!tw3se.asi and bin\\x64_dx12\\tw3se\\ (the extender, its README,
                              CHANGELOG and manual) - no example plugin; and fomod\\ for Vortex (see fomod()),
                              the only thing a hand unpack puts in the game's root
    TW3SE-SDK-<version>.zip   for plugin authors: headers, the two example plugins, the manual

    python tools/nexus_zip.py            -> build\\nexus\\<version>\\ (+ a .sha256 per zip)
"""
import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL = os.path.join(ROOT, "build", "release")
NOTICES = os.path.join(ROOT, "deps", "THIRD_PARTY_NOTICES.txt")  # tw3se.dll has MinHook (BSD 2-Clause) and Lua (MIT) in it


def version():
    h = open(os.path.join(ROOT, "include", "tw3se", "tw3se.h"), encoding="utf-8").read()
    parts = [re.search(rf"#define TW3SE_VERSION_{k}\s+(\d+)", h).group(1) for k in ("MAJOR", "MINOR", "PATCH")]
    return ".".join(parts)


OUT = os.path.join(ROOT, "build", "nexus", version())   # one folder per version: zips, .sha256, VirusTotal, test log


def add_tree(z, src, dst):
    for r, _d, files in os.walk(src):
        for f in files:
            p = os.path.join(r, f)
            z.write(p, os.path.join(dst, os.path.relpath(p, src)))


def fomod(z, v):
    """fomod\\ for Vortex. Without it Vortex's "dinput" installer takes any archive with a dinput8.dll and puts the
    files beside the exe the game entry names, bin\\x64 (Witcher 3 extension 1.6.10). The FOMOD installer comes
    first and keeps bin\\x64_dx12 - but then no mod type fits bin\\... and Vortex deployed it into Mods\\bin (Vortex
    1.12.6). So the FOMOD also puts a dinput8.dll at the mod's top: Vortex's "dinput" mod type takes
    the mod and deploys it into the game folder (tested: bin\\x64_dx12\\... right, TW3SE 1.2.0 started; an
    unused dinput8.dll lies in the game folder too)."""
    z.writestr("fomod/info.xml", f"""<?xml version="1.0" encoding="UTF-8"?>
<fomod>
  <Name>TW3SE</Name>
  <Author>MaximdeWinter</Author>
  <Version>{v}</Version>
</fomod>
""")
    z.writestr("fomod/ModuleConfig.xml", """<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>TW3SE</moduleName>
  <requiredInstallFiles>
    <folder source="bin" destination="bin" />
    <file source="bin\\x64_dx12\\dinput8.dll" destination="dinput8.dll" />
  </requiredInstallFiles>
</config>
""")


def seal(path):
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    open(path + ".sha256", "w", newline="\n").write(f"{h}  {os.path.basename(path)}\n")   # LF, for sha256sum -c
    print(f"{path}  {os.path.getsize(path) / 1e3:.0f} KB  sha256 {h[:16]}...")


def staged(v, game):
    """build\\release is package.bat's: the zips take it only when it holds this build and this version (an older
    staging would go into a zip with the new name)."""
    import filecmp
    for rel in (("dinput8.dll",), ("tw3se", "tw3se.dll")):
        built = os.path.join(ROOT, "build", *rel)
        put = os.path.join(game, "bin", "x64_dx12", *rel)
        if not os.path.exists(put) or not filecmp.cmp(built, put, shallow=False):
            sys.exit(f"build\\release has another {rel[-1]} than build\\ - run package.bat first")
    log = open(os.path.join(game, "CHANGELOG.md"), encoding="utf-8").read()
    if not re.search(rf"^##\s+{re.escape(v)}\s*$", log, re.M):
        sys.exit(f"build\\release has no CHANGELOG entry for {v} - run package.bat first")


def main():
    v = version()
    os.makedirs(OUT, exist_ok=True)
    game = os.path.join(REL, "TW3SE")
    staged(v, game)
    player = os.path.join(OUT, f"TW3SE-{v}.zip")
    with zipfile.ZipFile(player, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(game, "bin", "x64_dx12", "dinput8.dll"), r"bin\x64_dx12\dinput8.dll")
        z.write(os.path.join(game, "bin", "x64_dx12", "tw3se", "tw3se.dll"), r"bin\x64_dx12\tw3se\tw3se.dll")
        # 1.2: TW3SE's start under another mod's ASI loader, and Witcher Native Bridge's functions for its mods
        z.write(os.path.join(game, "bin", "x64_dx12", "!tw3se.asi"), r"bin\x64_dx12\!tw3se.asi")
        add_tree(z, os.path.join(game, "bin", "x64_dx12", "tw3se", "compat"), r"bin\x64_dx12\tw3se\compat")
        z.write(os.path.join(game, "README.md"), r"bin\x64_dx12\tw3se\README.md")
        z.write(os.path.join(game, "CHANGELOG.md"), r"bin\x64_dx12\tw3se\CHANGELOG.md")
        add_tree(z, os.path.join(game, "docs"), r"bin\x64_dx12\tw3se\docs")
        add_tree(z, os.path.join(game, "bin", "x64_dx12", "tw3se", "lua"), r"bin\x64_dx12\tw3se\lua")   # library, examples, tests
        z.write(NOTICES, r"bin\x64_dx12\tw3se\THIRD_PARTY_NOTICES.txt")
        fomod(z, v)
    seal(player)
    sdk = os.path.join(OUT, f"TW3SE-SDK-{v}.zip")
    with zipfile.ZipFile(sdk, "w", zipfile.ZIP_DEFLATED) as z:
        add_tree(z, os.path.join(REL, "TW3SE-SDK"), "TW3SE-SDK")
        z.write(NOTICES, r"TW3SE-SDK\THIRD_PARTY_NOTICES.txt")
    seal(sdk)


if __name__ == "__main__":
    main()
