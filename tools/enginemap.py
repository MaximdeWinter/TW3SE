"""The engine's map, read from witcher3.exe: every function, every C++ class with a vtable (MSVC RTTI: its bases and
slots), every direct call, every text a function points at, and the names TW3SE knows (tw3se\\out\\names.txt: the
address list and every native's handler). Built once per build, then searched.

    python tools/enginemap.py build <witcher3.exe> [--names <names.txt>]     -> _cache/enginemap/<build>.npz + .json
    python tools/enginemap.py find <text>          classes, names and texts that hold <text>
    python tools/enginemap.py class <Name>         bases, vtables, each slot: function, name, where it is overridden
    python tools/enginemap.py fn <rva|name>        a function: names, vtable slots, texts, callers, callees
    python tools/enginemap.py stats

The map of the newest build is used unless --build <hex> is given. RVAs are hex, relative to the image base.
Needs Python 3 with numpy, pefile and capstone (pip install numpy pefile capstone).
"""
import argparse
import bisect
import glob
import json
import os
import re
import struct
import sys
import time

import numpy as np
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "_cache", "enginemap")


def demangle(raw):
    """'.?AVCFoo@Bar@@' style without the decoration: 'Bar::CFoo'; templates keep their MSVC form."""
    if raw.startswith("?$"):
        return raw
    parts = [p for p in raw.split("@") if p]
    return "::".join(reversed(parts))


# ---------------------------------------------------------------------------------------------------- building
def build(exe, names_path):
    t0 = time.time()
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    stamp = "%08x" % pe.FILE_HEADER.TimeDateStamp
    sec = {s.Name.rstrip(b"\0").decode(): (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections}
    ta, tb = sec[".text"]
    ra, rb = sec[".rdata"]
    print(f"build {stamp}, .text {tb - ta:,} bytes, .rdata {rb - ra:,} bytes")

    # functions: .pdata, chained entries (a function's cold parts) point at their parent
    starts, ends, parent = [], [], []
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        b, en, u = e.struct.BeginAddress, e.struct.EndAddress, e.struct.UnwindData
        par = 0
        if u and u + 4 <= len(img):
            flags = img[u] >> 3
            if flags & 4:  # UNW_FLAG_CHAININFO: a RUNTIME_FUNCTION follows the (even) code array
                n = img[u + 2]
                p = u + 4 + ((n + 1) & ~1) * 2
                if p + 12 <= len(img):
                    par = struct.unpack_from("<I", img, p)[0]
        starts.append(b), ends.append(en), parent.append(par)
    order = np.argsort(starts)
    fstart = np.array(starts, np.uint32)[order]
    fend = np.array(ends, np.uint32)[order]
    fparent = np.array(parent, np.uint32)[order]
    # a chained part's parent may itself be chained: follow to the top
    idx = {int(s): i for i, s in enumerate(fstart)}
    for i in range(len(fparent)):
        p, hops = int(fparent[i]), 0
        while p and hops < 8 and idx.get(p) is not None and fparent[idx[p]]:
            p, hops = int(fparent[idx[p]]), hops + 1
        fparent[i] = p
    print(f"functions: {len(fstart):,} ({int((fparent != 0).sum()):,} chained parts)")

    # MSVC RTTI: type descriptors by name, complete object locators by a vectorized scan of .rdata
    tds = {}
    for m in re.finditer(rb"\.\?A[VU]([A-Za-z0-9_@?$<>,]+?)@@\x00", img):  # (they lie in .data)
        tds[m.start() - 0x10] = m.group(1).decode("ascii", "replace")
    rd = np.frombuffer(img[ra:rb - ((rb - ra) % 4)], dtype="<u4")
    cand = np.nonzero(rd[:-6] == 1)[0]
    selfrva = ra + cand * 4
    ok = rd[cand + 5] == selfrva
    cols = {}
    for i in cand[ok]:
        td = int(rd[i + 3])
        if td in tds:
            cols[ra + int(i) * 4] = (td, int(rd[i + 1]), int(rd[i + 4]))  # type, offset in the object, hierarchy
    # vtables: the qword before slot 0 points at the COL
    n8 = (rb - ra) // 8
    q = np.frombuffer(img[ra:ra + n8 * 8], dtype="<u8").astype(np.int64) - base
    colarr = np.array(sorted(cols), np.int64)
    hit = np.nonzero(np.isin(q, colarr))[0]
    classes = {}
    nslots = 0
    for i in hit:
        col = int(q[i])
        td, off, chd = cols[col]
        name = demangle(tds[td])
        vt = ra + (int(i) + 1) * 8
        slots = []
        p = vt
        while p + 8 <= rb:
            f = struct.unpack_from("<Q", img, p)[0] - base
            if not (ta <= f < tb) or (p != vt and (p - ra) // 8 in hitset_cache(hit)):
                break
            slots.append(f)
            p += 8
        nslots += len(slots)
        c = classes.setdefault(name, {"bases": None, "vtables": []})
        c["vtables"].append({"rva": vt, "offset": off, "slots": slots})
        if c["bases"] is None:
            c["bases"] = hierarchy(img, chd, tds)
    print(f"classes with a vtable: {len(classes):,}, vtables {sum(len(c['vtables']) for c in classes.values()):,}, "
          f"slots {nslots:,}")

    # direct calls: E8 rel32 that land on a function start
    text = np.frombuffer(img[ta:tb], dtype=np.uint8)
    pos = np.nonzero(text[:-5] == 0xE8)[0]
    rel = (text[pos + 1].astype(np.int64) | (text[pos + 2].astype(np.int64) << 8) |
           (text[pos + 3].astype(np.int64) << 16) | (text[pos + 4].astype(np.int64) << 24))
    rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
    tgt = ta + pos + 5 + rel
    keep = np.isin(tgt, fstart.astype(np.int64))
    csite = (ta + pos[keep]).astype(np.uint32)
    ctgt = tgt[keep].astype(np.uint32)
    ccaller = owner_of(fstart, fend, fparent, csite)
    print(f"direct calls: {len(csite):,}")

    # texts: lea r64, [rip+disp32] (48/4C 8D, modrm mod 00 rm 101) into .rdata, at a printable C string
    lp = np.nonzero((text[1:-6] == 0x8D) & ((text[:-7] & 0xFB) == 0x48) & ((text[2:-5] & 0xC7) == 0x05))[0]
    disp = (text[lp + 3].astype(np.int64) | (text[lp + 4].astype(np.int64) << 8) |
            (text[lp + 5].astype(np.int64) << 16) | (text[lp + 6].astype(np.int64) << 24))
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    st = ta + lp + 7 + disp
    inr = (st >= ra) & (st < rb)
    lp, st = lp[inr], st[inr]
    texts, tsite, tid = {}, [], []
    for p_, s_ in zip(lp.tolist(), st.tolist()):
        if s_ not in texts:
            texts[s_] = cstring(img, s_)
        if texts[s_] is not None:
            tsite.append(ta + p_)
            tid.append(s_)
    texts = {k: v for k, v in texts.items() if v is not None}
    tsite = np.array(tsite, np.uint32)
    tfn = owner_of(fstart, fend, fparent, tsite)
    print(f"texts: {len(texts):,} strings at {len(tsite):,} places")

    names = {}
    if names_path and os.path.exists(names_path):
        for line in open(names_path, encoding="utf-8", errors="replace"):
            if line.startswith("#") or " " not in line:
                continue
            r, n = line.rstrip("\n").split(" ", 1)
            names.setdefault(int(r, 16), []).append(n)
    print(f"names: {sum(len(v) for v in names.values()):,} from {names_path}")

    slotnames = name_slots(img, names, classes, fstart, fend, fparent)
    print(f"slots named by their natives: {sum(len(v) for v in slotnames.values()):,}")

    os.makedirs(OUT, exist_ok=True)
    np.savez_compressed(os.path.join(OUT, stamp + ".npz"), fstart=fstart, fend=fend, fparent=fparent,
                        csite=csite, ctgt=ctgt, ccaller=ccaller, tsite=tsite, tid=np.array(tid, np.uint32), tfn=tfn)
    json.dump({"build": stamp, "exe": os.path.abspath(exe), "classes": classes,
               "texts": {str(k): v for k, v in texts.items()}, "names": {str(k): v for k, v in names.items()},
               "slotnames": slotnames},
              open(os.path.join(OUT, stamp + ".json"), "w", encoding="utf-8"))
    print(f"-> {OUT}\\{stamp}.* in {time.time() - t0:.0f} s")


def name_slots(img, names, classes, fstart, fend, fparent):
    """A native of a class whose handler makes calls through one vtable slot only (call/jmp [reg+disp]) names that
    slot: CEntity's native TeleportWithRotation calls [rax+0x460], so CEntity slot 140 is TeleportWithRotation.
    Measured on CEntity.TeleportWithRotation (two calls, both +0x460). -> {class: {slot: method}}"""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out = {}
    for r, ns in names.items():
        for n in ns:
            mm = re.match(r"native ([A-Za-z0-9_]+)\.([A-Za-z0-9_]+)$", n)
            if not mm or mm.group(1) not in classes:
                continue
            cls, meth = mm.group(1), mm.group(2)
            i = int(np.searchsorted(fstart, r, side="right")) - 1
            if i < 0 or fstart[i] != r:
                continue
            parts = [(int(fstart[i]), int(fend[i]))]
            disps = set()
            for a_, b_ in parts:
                for ins in md.disasm(img[a_:b_], a_):
                    if ins.mnemonic in ("call", "jmp") and len(ins.operands) == 1:
                        op = ins.operands[0]
                        if op.type == capstone.x86.X86_OP_MEM and op.mem.base not in (0, capstone.x86.X86_REG_RIP)                                 and op.mem.index == 0 and op.mem.disp >= 0 and op.mem.disp % 8 == 0:
                            disps.add(op.mem.disp // 8)
            v0 = next((v for v in classes[cls]["vtables"] if v["offset"] == 0), None)
            if len(disps) == 1 and v0 and next(iter(disps)) < len(v0["slots"]):
                d = out.setdefault(cls, {})
                k = str(next(iter(disps)))  # several natives on one slot (Teleport, TeleportWithRotation): all
                d[k] = "|".join(sorted(set((d.get(k) or "").split("|") + [meth]) - {""}))
    return out


_hitset = None


def hitset_cache(hit):
    """vtable starts (as .rdata qword indexes + 1) - a vtable ends where the next one's COL pointer stands"""
    global _hitset
    if _hitset is None:
        _hitset = set((hit).tolist())
    return _hitset


def hierarchy(img, chd, tds):
    """the base classes from the class hierarchy descriptor, most derived first (the class itself left out)"""
    try:
        sig, attr, n, bca = struct.unpack_from("<IIII", img, chd)
        out = []
        for k in range(1, min(n, 64)):
            bcd = struct.unpack_from("<I", img, bca + 4 * k)[0]
            td = struct.unpack_from("<I", img, bcd)[0]
            if td in tds:
                out.append(demangle(tds[td]))
        return out
    except struct.error:
        return []


def cstring(img, rva):
    end = img.find(b"\0", rva, rva + 300)
    if end < 0 or end - rva < 4:
        return None
    s = img[rva:end]
    if not all(32 <= c < 127 or c in (9, 10) for c in s):
        return None
    return s.decode("ascii")


def owner_of(fstart, fend, fparent, sites):
    """the function each site lies in (a chained part counts for its parent); 0 outside every function"""
    i = np.searchsorted(fstart, sites, side="right") - 1
    i = np.clip(i, 0, len(fstart) - 1)
    inside = (sites >= fstart[i]) & (sites < fend[i])
    own = np.where(fparent[i] != 0, fparent[i], fstart[i])
    return np.where(inside, own, 0).astype(np.uint32)


# ---------------------------------------------------------------------------------------------------- the map
class Map:
    def __init__(self, stamp=None):
        files = sorted(glob.glob(os.path.join(OUT, "*.json")), key=os.path.getmtime)
        if stamp:
            files = [f for f in files if os.path.basename(f).startswith(stamp)]
        if not files:
            sys.exit("no map: python tools/enginemap.py build <witcher3.exe> --names <names.txt>")
        j = json.load(open(files[-1], encoding="utf-8"))
        self.build = j["build"]
        self.classes = j["classes"]
        self.texts = {int(k): v for k, v in j["texts"].items()}
        self.names = {int(k): v for k, v in j["names"].items()}
        z = np.load(files[-1][:-5] + ".npz")
        for k in z.files:
            setattr(self, k, z[k])
        self.slotnames = {c: {int(k): v for k, v in d.items()} for c, d in j.get("slotnames", {}).items()}
        self.slots_of = {}  # function -> [(class, vtable offset, slot)]
        for cn, c in self.classes.items():
            for v in c["vtables"]:
                for i, f in enumerate(v["slots"]):
                    self.slots_of.setdefault(f, []).append((cn, v["offset"], i))

    def func_at(self, rva):
        i = bisect.bisect_right(self.fstart, rva) - 1
        if i < 0 or rva >= self.fend[i]:
            return None
        return int(self.fparent[i]) or int(self.fstart[i])

    def slot_name(self, cn, i):
        for c in [cn] + (self.classes.get(cn, {}).get("bases") or []):
            n = self.slotnames.get(c, {}).get(i)
            if n:
                return n
        return None

    def label(self, f):
        """the best name of a function: TW3SE's name, else the most basic class whose slot it fills"""
        if f in self.names:
            return self.names[f][0]
        s = self.slots_of.get(f)
        if s:
            cn, off, i = min(s, key=lambda x: (len(self.classes[x[0]]["bases"] or []), x[0]))
            i0 = bisect.bisect_right(self.fstart, f) - 1
            stub = i0 >= 0 and self.fend[i0] - self.fstart[i0] < 16  # (a stub shared by many classes: slot form)
            if not off and not stub:
                n = self.slot_name(cn, i)
                if n and "|" not in n:  # (several natives on one slot: a helper they share, no name)
                    return f"{cn}::{n}"
            return f"{cn}::vf{i}" + (f"@{off:x}" if off else "")
        return f"sub_{f:x}"

    def class_slots(self, name):
        c = self.classes[name]
        rows = []
        bases = c["bases"] or []
        for v in c["vtables"]:
            for i, f in enumerate(v["slots"]):
                from_base = None
                for b in bases:  # the nearest base with the same function in this slot: inherited
                    bc = self.classes.get(b)
                    if bc and any(bv["offset"] == 0 and i < len(bv["slots"]) and bv["slots"][i] == f
                                  for bv in bc["vtables"]) and v["offset"] == 0:
                        from_base = b
                        break
                rows.append((v["offset"], i, f, from_base))
        return rows


def cmd_find(m, text):
    t = text.lower()
    hits = [c for c in m.classes if t in c.lower()]
    print(f"classes ({len(hits)}):")
    for c in sorted(hits)[:60]:
        print("  ", c)
    nh = [(r, n) for r, ns in m.names.items() for n in ns if t in n.lower()]
    print(f"names ({len(nh)}):")
    for r, n in sorted(nh, key=lambda x: x[1])[:60]:
        print(f"   {r:x}  {n}")
    th = [(r, s) for r, s in m.texts.items() if t in s.lower()]
    print(f"texts ({len(th)}):")
    for r, s in th[:40]:
        users = sorted(set(int(f) for f in m.tfn[m.tid == r] if f))
        print(f"   {s[:90]!r}  <- " + ", ".join(m.label(f) for f in users[:4]))


def cmd_class(m, name):
    if name not in m.classes:
        near = [c for c in m.classes if name.lower() in c.lower()]
        sys.exit("no class " + name + (": " + ", ".join(near[:20]) if near else ""))
    c = m.classes[name]
    print(name)
    print("  bases:", " <- ".join(c["bases"] or []) or "-")
    for v in c["vtables"]:
        print(f"  vtable {v['rva']:x} (offset {v['offset']:x}): {len(v['slots'])} slots")
    for off, i, f, from_base in m.class_slots(name):
        own = f"(from {from_base})" if from_base else "OWN"
        sn = m.slot_name(name, i) if not off else None
        print(f"  {off:4x} vf{i:<4} {f:8x}  {own:30} {m.label(f)}" +
              (f"   [used by the natives {sn.replace('|', ', ')}]" if sn and "|" in sn else ""))


def cmd_fn(m, what):
    try:
        f = int(what, 16)
    except ValueError:
        f = next((r for r, ns in m.names.items() if what in ns), None)
        if f is None:
            sys.exit("no function named " + what)
    f0 = m.func_at(f)
    if f0 is None:
        sys.exit(f"{f:x} lies in no function")
    print(f"{f0:x}  {m.label(f0)}")
    for n in m.names.get(f0, []):
        print("  name:", n)
    for cn, off, i in m.slots_of.get(f0, [])[:30]:
        print(f"  slot: {cn} vf{i}" + (f" (vtable at offset {off:x})" if off else ""))
    ts = sorted(set(int(x) for x in m.tid[m.tfn == f0]))
    for r in ts[:30]:
        print(f"  text: {m.texts[r][:110]!r}")
    callers = sorted(set(int(x) for x in m.ccaller[m.ctgt == f0] if x))
    print(f"  called by {len(callers)}:", ", ".join(m.label(c) for c in callers[:25]))
    callees = sorted(set(int(x) for x in m.ctgt[m.ccaller == f0]))
    print(f"  calls {len(callees)}:", ", ".join(m.label(c) for c in callees[:40]))


def cmd_stats(m):
    named = len(m.names)
    slotted = len(m.slots_of)
    print(f"build {m.build}: {len(m.fstart):,} functions ({int((m.fparent == 0).sum()):,} without chained parts), "
          f"{len(m.classes):,} classes, {slotted:,} functions in a vtable slot, {named:,} named by TW3SE, "
          f"{len(m.texts):,} texts, {len(m.csite):,} direct calls")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--names")
    ap.add_argument("--build")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if a.cmd == "build":
        build(a.arg, a.names)
        return
    m = Map(a.build)
    {"find": cmd_find, "class": cmd_class, "fn": cmd_fn}.get(a.cmd, lambda m, _: cmd_stats(m))(m, a.arg)


if __name__ == "__main__":
    main()
