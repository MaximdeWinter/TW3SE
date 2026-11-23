"""Patched skinning vertex shaders from a shader dump: each vertex shader that reads BLENDINDICES is disassembled
(DXC 1.9), the skinned position found and the inject patched in (tools/skin_patch.py), assembled and signed again
(tools/dxil_asm). The results go to <out>/<id>.cso, ready for tw3se\\shaders\\.

    python tools/skin_build.py <dump folder> <out folder> <inject>       inject: see tools/skin_patch.py

<out>/skinhook.txt lists the ids patched: TW3SE (skin_hook = on) sets the slot of the mesh drawn for these. With
inject capsules the ray tracing skinning compute shaders of the dump are patched too (inject rt).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DXC = os.path.join(ROOT, "_cache", "dxc19", "bin", "x64", "dxc.exe")
ASM = os.path.join(ROOT, "tools", "dxil_asm", "dxil_asm.exe")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import skin_patch  # noqa: E402


def main(dump, out, inject):
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, "_ll")
    os.makedirs(work, exist_ok=True)
    made, skipped, failed = [], [], []
    for f in sorted(os.listdir(dump)):
        compute = f.endswith(".cs.cso")  # the ray tracing skinning (inject rt), with the capsules only
        if not f.endswith(".vs.cso") and not (compute and inject == "capsules"):
            continue
        data = open(os.path.join(dump, f), "rb").read()
        if not compute and b"BLENDINDICES" not in data:
            continue
        sid = f.split(".")[0]
        ll = os.path.join(work, sid + ".ll")
        subprocess.run([DXC, "-dumpbin", os.path.join(dump, f), "-Fc", ll], capture_output=True)
        if not os.path.exists(ll):  # (DXC does not read it: not DXIL)
            failed.append((sid, "DXC does not read it"))
            continue
        text = open(ll, encoding="utf-8", errors="replace").read()
        body, lines = skin_patch.parse(text)
        if compute:
            if not skin_patch.find_rt(lines):
                continue
        elif not skin_patch.find_positions(body):
            skipped.append(sid)
            continue
        patched, pos = skin_patch.patch(text, "rt" if compute else inject)
        pll = os.path.join(work, sid + ".patched.ll")
        open(pll, "w", encoding="utf-8", newline="\n").write(patched)
        r = subprocess.run([ASM, pll, os.path.join(out, sid + ".cso")], capture_output=True, text=True)
        if r.returncode:
            failed.append((sid, (r.stderr or r.stdout).strip()[:300]))
        else:
            made.append((sid, len(pos)))
    # the material pixel shaders drawn with these vertex shaders (the dump's pipelines.txt): the skin overlay
    if inject == "capsules":
        import re as _re
        pl = os.path.join(dump, "pipelines.txt")
        pss = set()
        done_vs = {sid for sid, _ in made}
        if os.path.exists(pl):
            for line in open(pl, encoding="utf-8", errors="replace"):
                mv, mp = _re.search(r"vs=([0-9a-f]+)", line), _re.search(r"ps=([0-9a-f]+)", line)
                if mv and mp and mv.group(1) in done_vs:
                    pss.add(mp.group(1))
        nps = 0
        for pid in sorted(pss):
            f = os.path.join(dump, pid + ".ps.cso")
            if not os.path.exists(f):
                continue
            ll = os.path.join(work, pid + ".ps.ll")
            subprocess.run([DXC, "-dumpbin", f, "-Fc", ll], capture_output=True)
            if not os.path.exists(ll):
                continue
            try:
                txt = skin_patch.patch_skin_ps(open(ll, encoding="utf-8", errors="replace").read())
            except SystemExit:
                continue
            pll = os.path.join(work, pid + ".ps.patched.ll")
            open(pll, "w", encoding="utf-8", newline="\n").write(txt)
            r = subprocess.run([ASM, pll, os.path.join(out, pid + ".cso")], capture_output=True, text=True)
            if r.returncode == 0:
                nps += 1
            else:
                failed.append((pid, "ps: " + (r.stderr or r.stdout).strip()[:300]))
        print(f"pixel shaders with the overlay: {nps} of {len(pss)}")
    # the ids TW3SE's skinning hook sets the slot for (skin_hook = on reads tw3se\shaders\skinhook.txt); ids already
    # in the file stay (a folder of missing ones, tw3se\shaders\skinhook_missing, adds to a set)
    listing = os.path.join(out, "skinhook.txt")
    ids = []
    if os.path.exists(listing):
        ids = [l.strip() for l in open(listing, encoding="utf-8") if l.strip() and not l.startswith("#")]
    for sid, _ in made:
        if sid not in ids:
            ids.append(sid)
    with open(listing, "w", encoding="utf-8", newline="\n") as f:
        f.write("# skinning vertex shaders with TW3SE's hook (tools/skin_build.py, inject " + inject + ")\n")
        if inject == "capsules":  # TW3SE reads it: the table's layout has to be its own
            f.write("# layout capsules %d rows %d mush 1 overlay 2 dqs 1\n" % (skin_patch.CAPS, skin_patch.CAP_ROWS))
        for sid in ids:
            f.write(sid + "\n")
    print(f"patched {len(made)}, no position found {len(skipped)}, failed {len(failed)}")
    for sid, err in failed:
        print("  FAIL", sid, err)
    if skipped:
        print("  without a position:", " ".join(skipped))
    return 0 if not failed else 1


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
