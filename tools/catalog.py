"""Search the RTTI catalog the `catalog` console command writes (tw3se\\catalog\\rtti.json).

    python catalog.py find <text>        classes, enums and functions whose name contains <text>
    python catalog.py class <Name>       a class with its parents, fields and methods (inherited ones too: --all)
    python catalog.py enum <Name>        an enum's values
    python catalog.py fn <Name>          global functions and methods of that name
    python catalog.py callable <Name>    a class's methods, split into callable-now from Lua vs not (incl. inherited)
    python catalog.py check              compare the catalog with the game's scripts (enums, returns, flags)
    python catalog.py diff <old> <new>   what a patch changed between two rtti.json (types, functions, fields, sizes)

Where the catalog comes from: --file <path>, or the game folder (--game <folder> or the variable TW3SE_GAME) with
bin\\x64_dx12\\tw3se\\catalog\\rtti.json in it. `check` reads the game's scripts from the same game folder.
"""
import sys, os, re, glob, json, collections

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "_cache", "catalog", "rtti.json")  # the repository's copy (tools/catalog_run.py)
GAME = os.environ.get("TW3SE_GAME", "")


def catalog_path():
    if GAME:
        return os.path.join(GAME, "bin", "x64_dx12", "tw3se", "catalog", "rtti.json")
    if os.path.exists(CACHE):
        return CACHE
    sys.exit("no catalog: run the console command `catalog` in the game, then pass --game <game folder> "
             "(or set TW3SE_GAME), or --file <rtti.json>")

KINDS = {0: "simple", 1: "enum", 2: "class", 3: "array", 4: "static array", 5: "native array", 6: "pointer",
         7: "handle", 8: "soft handle", 9: "bitfield", 11: "fundamental", 12: "map"}

# CFunction+0x18 bits, read off the script declarations by `check` (5.01: each modifier sets its bit in
# 95-100% of its functions). Also: bit 0 native, bit 1 global, bit 16 script method, bit 21 imported this run.
FN_BITS = {14: "private", 15: "protected", 3: "import", 4: "final", 6: "latent", 8: "exec", 12: "quest",
           10: "timer", 7: "entry", 11: "storyscene", 13: "cleanup", 17: "reward"}
FN_EVENT = 5
# CProperty+0x24 bits, read off the script `var` modifiers by `check` (5.01: each modifier sets its bit in
# >=97% of its vars). Bit 5 (0x20) marks a script-declared property (set on nearly every scripted var).
PROP_BITS = {0: "editable", 1: "const", 2: "inlined", 5: "scripted", 14: "import", 15: "saved",
             16: "private", 17: "protected", 18: "public"}
WS_TYPES = {"Int32": "int", "Float": "float", "Bool": "bool", "String": "string", "CName": "name", "Uint8": "byte"}


def prop_mods(flags):
    return [n for b, n in sorted(PROP_BITS.items()) if flags >> b & 1]


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    d["byName"] = {t["name"]: t for t in d["types"]}
    return d


def ws_type(t):
    """Engine type name -> how a script writes it (handle:CActor -> CActor, array:2,0,X -> array<X>)."""
    if not t:
        return ""
    if t.startswith("handle:"):
        return ws_type(t[7:])
    if t.startswith("array:"):
        return "array<" + ws_type(t.split(",", 2)[2]) + ">"
    return WS_TYPES.get(t, t)


def flag_names(flags):
    return [n for b, n in FN_BITS.items() if flags >> b & 1]


def signature(f, owner=None):
    params = ", ".join(f"{p['name']} : {ws_type(p['type'])}" for p in f["params"])
    ret = f" : {ws_type(f['ret'])}" if f.get("ret") else ""
    words = flag_names(f["flags"]) + ["event" if f["flags"] >> FN_EVENT & 1 else "function"]
    if words[-1] == "event":
        ret = ""  # an event always returns bool
    over = f"  (overrides {f['overrides']})" if f.get("overrides") else ""
    name = f"{owner}.{f['name']}" if owner else f["name"]
    return f"{' '.join(words)} {name}({params}){ret}{over}"


def chain(d, name):
    out = []
    while name and name in d["byName"] and len(out) < 64:
        out.append(d["byName"][name])
        name = d["byName"][name].get("parent")
    return out


def cmd_find(d, text):
    t = text.lower()
    for ty in d["types"]:
        if t in ty["name"].lower() and ty["kind"] in (1, 2, 9):
            extra = f" : {ty['parent']}" if ty.get("parent") else ""
            print(f"{KINDS.get(ty['kind'], ty['kind']):6} {ty['name']}{extra}")
    for f in d["functions"]:
        if t in f["name"].lower():
            print("global", signature(f))
    for ty in d["types"]:
        for m in ty.get("methods", []):
            if t in m["name"].lower():
                print("method", signature(m, ty["name"]))


def cmd_class(d, name, inherited):
    classes = chain(d, name)
    if not classes:
        print(f"no class {name}")
        return
    print(" -> ".join(c["name"] for c in classes), f"(size {classes[0].get('size')})")
    for c in classes if inherited else classes[:1]:
        print(f"\n[{c['name']}] {len(c['props'])} fields, {len(c['methods'])} methods")
        for p in sorted(c["props"], key=lambda p: p["offset"]):
            mods = prop_mods(p["flags"]) or ["native"]
            print(f"  +0x{p['offset']:<5x} {p['name']} : {ws_type(p['type'])}  ({' '.join(mods)})")
        for m in c["methods"]:
            print("  " + signature(m))


def cmd_enum(d, name):
    e = d["byName"].get(name)
    if not e or e["kind"] != 1:
        print(f"no enum {name}")
        return
    print(f"enum {name} (size {e.get('size')})")
    for n, v in e.get("values", []):
        print(f"  {n} = {v}")


def cmd_fn(d, name):
    for f in d["functions"]:
        if f["name"] == name:
            print("global", signature(f), f"flags 0x{f['flags']:x}")
    for ty in d["types"]:
        for m in ty.get("methods", []):
            if m["name"] == name:
                print("method", signature(m, ty["name"]), f"flags 0x{m['flags']:x}")


def why_not_callable(m, d):
    """Why Lua cannot call this method yet, or None when it can. Mirrors lua.cpp's rules (arrays and
    out-scalars are callable; an out-struct, a map and exotic pointer types are not)."""
    if m["flags"] & 0x40:
        return "latent"
    for p in m["params"]:
        t = p["type"]
        if t.startswith("map:"):
            return "map argument (" + p["name"] + ")"
        if (p.get("flags", 0) & 0x200) and not t.startswith("array:"):  # bit 9 = out; an out-scalar is fine
            base = d["byName"].get(t)
            if base and base.get("kind") == 2:  # an out-struct is not supported yet (bigger than one slot)
                return "out/by-reference struct (" + p["name"] + ")"
        base = t[7:] if t.startswith("handle:") else t
        if t.startswith("ptr:") or t.startswith("soft:") or t.startswith("static:") or base.startswith("["):
            return "argument type " + t + " (" + p["name"] + ")"
    return None


def cmd_callable(d, name):
    classes = chain(d, name)
    if not classes:
        print(f"no class {name}")
        return
    ok, no = [], []
    for c in classes:
        for m in c["methods"]:
            (no if why_not_callable(m, d) else ok).append((c["name"], m))
    print(f"{name}: {len(ok)} method(s) callable from Lua now, {len(no)} not yet\n")
    print("-- callable now --")
    for owner, m in ok:
        print(f"  {signature(m, owner)}")
    print("\n-- not yet --")
    for owner, m in no:
        print(f"  {signature(m, owner)}   [{why_not_callable(m, d)}]")


def read_scripts():
    if not GAME:
        sys.exit("check reads the game's scripts: pass --game <game folder> or set TW3SE_GAME")
    scripts = os.path.join(GAME, "content", "content0", "scripts")
    src = []
    for p in glob.glob(os.path.join(scripts, "**", "*.ws"), recursive=True):
        raw = open(p, "rb").read()
        enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
        src.append(raw.decode(enc, errors="replace"))
    return re.sub(r"//[^\n]*", "", "\n".join(src))


def cmd_check(d):
    src = read_scripts()
    # enums
    ws_enums = {}
    for m in re.finditer(r"\benum\s+(\w+)\s*\{([^}]*)\}", src):
        vals, cur = {}, -1
        for item in [x.strip() for x in m.group(2).split(",") if x.strip()]:
            mm = re.match(r"(\w+)\s*(?:=\s*(-?(?:0x[0-9a-fA-F]+|\d+)))?\s*$", item)
            if not mm:
                vals = None
                break
            cur = int(mm.group(2), 0) if mm.group(2) else cur + 1
            vals[mm.group(1)] = cur
        if vals is not None:
            ws_enums[m.group(1)] = vals
    ok = [n for n, v in ws_enums.items() if n in d["byName"] and dict(d["byName"][n].get("values", [])) == v]
    print(f"enums: {len(ok)} of {sum(1 for n in ws_enums if n in d['byName'])} match the scripts")

    # functions: return type, modifiers -> flag bits
    norm = {"int": "Int32", "float": "Float", "bool": "Bool", "string": "String", "name": "CName", "byte": "Uint8"}
    decl = re.compile(r"((?:\b(?:import|private|protected|public|final|latent|exec|quest|timer|entry|storyscene|"
                      r"cleanup|reward|saved)\s+)*)\b(function|event)\s+(\w+)\s*\(([^)]*)\)\s*"
                      r"(?::\s*(array\s*<\s*\w+\s*>|\w+))?")
    cls_re = re.compile(r"\b(?:state\s+(\w+)\s+in\s+(\w+)|class\s+(\w+))")
    owners = []  # (position, class name) to know which class a declaration is in
    for m in cls_re.finditer(src):
        owners.append((m.start(), (m.group(2) + "State" + m.group(1)) if m.group(1) else m.group(3)))
    methods = {(t["name"], m["name"]): m for t in d["types"] for m in t.get("methods", [])}
    globals_ = {f["name"]: f for f in d["functions"]}
    bit_by_word = collections.defaultdict(collections.Counter)
    words_total = collections.Counter()
    ret_ok = ret_bad = 0
    ret_diff, ret_examples = collections.Counter(), []
    import bisect
    starts = [p for p, _ in owners]
    for m in decl.finditer(src):
        words = set(m.group(1).split()) | {m.group(2)}
        name = m.group(3)
        i = bisect.bisect_right(starts, m.start()) - 1
        depth = src.count("{", owners[i][0], m.start()) - src.count("}", owners[i][0], m.start()) if i >= 0 else 0
        f = methods.get((owners[i][1], name)) if i >= 0 and depth > 0 else globals_.get(name)
        if not f:
            continue
        exp = m.group(5)
        if m.group(2) == "event":
            exp = "bool"  # an event returns bool, declared or not
        if exp == "void":
            exp = None
        if exp:
            a = re.match(r"array\s*<\s*(\w+)\s*>", exp)
            exp = ("array:2,0," + norm.get(a.group(1), a.group(1))) if a else norm.get(exp, exp)
        got = f.get("ret") or None
        if got and got.startswith("array:2,0,handle:"):
            got = "array:2,0," + got[17:]
        if got and got.startswith("handle:"):
            got = got[7:]
        if exp == got:
            ret_ok += 1
        else:
            ret_bad += 1
            ret_diff[("import" in words, bool(exp), bool(got))] += 1
            if len(ret_examples) < 8 and "import" not in words:
                ret_examples.append((owners[i][1] if i >= 0 and depth > 0 else "-", name, exp, got))
        for w in words:
            words_total[w] += 1
            for b in range(32):
                if f["flags"] >> b & 1:
                    bit_by_word[w][b] += 1
    print(f"return types: {ret_ok} match, {ret_bad} differ")
    for (imp, has_ws, has_cat), n in ret_diff.most_common():
        print(f"  {n:5}  {'import' if imp else 'script'}: script declares {'a' if has_ws else 'no'} return, "
              f"catalog has {'one' if has_cat else 'none'}")
    for e in ret_examples:
        print("   ", e)
    print("modifier -> flag bits set in at least 95% of its functions:")
    for w, n in words_total.most_common():
        bits = [b for b, c in bit_by_word[w].items() if c >= 0.95 * n]
        print(f"  {w:10} {n:6}  bits {sorted(bits)}")
    # overrides: the overridden function's class is an ancestor
    good = bad = 0
    for t in d["types"]:
        anc = {c["name"] for c in chain(d, t.get("parent"))} if t.get("parent") else set()
        for m in t.get("methods", []):
            if m.get("overrides"):
                good += m["overrides"] in anc
                bad += m["overrides"] not in anc
    print(f"overrides: {good} point to a parent class, {bad} do not")

    # property modifiers: a script `var` with modifiers vs the catalog prop's CProperty+0x24 bits
    prop_flags = {(t["name"], p["name"]): p["flags"] for t in d["types"] for p in t.get("props", [])}
    var_re = re.compile(r"((?:\b(?:editable|saved|const|import|inlined|private|protected|public)\s+)*)\bvar\s+(\w+)\s*:")
    pmod_total, pmod_hit = collections.Counter(), collections.Counter()
    matched = 0
    for m in var_re.finditer(src):
        words = {w.lower() for w in m.group(1).split()}
        if not words:
            continue
        i = bisect.bisect_right(starts, m.start()) - 1
        if i < 0:
            continue
        depth = src.count("{", owners[i][0], m.start()) - src.count("}", owners[i][0], m.start())
        if depth != 1:  # a class-level var, not a local inside a method
            continue
        fl = prop_flags.get((owners[i][1], m.group(2)))
        if fl is None:
            continue
        matched += 1
        for w in words:
            pmod_total[w] += 1
            if w in PROP_BITS.values():
                bit = next(b for b, n in PROP_BITS.items() if n == w)
                pmod_hit[w] += (fl >> bit & 1)
    print(f"property modifiers: {matched} script vars matched to the catalog")
    for w, n in pmod_total.most_common():
        if w in PROP_BITS.values():
            print(f"  {w:10} {pmod_hit[w]:6}/{n:<6} have the expected bit")


def fn_sig(f):
    """A function's signature as a comparable tuple: parameters (name + type), return type, flags."""
    return (tuple((p["name"], p["type"]) for p in f.get("params", [])), f.get("ret"), f.get("flags"))


def cmd_diff(old, new):
    """What a patch changed between two catalogs: types and functions added/removed, members and sizes changed."""
    print("build %s -> %s" % (old.get("build"), new.get("build")))
    oset, nset = set(old["byName"]), set(new["byName"])
    added_t, removed_t = sorted(nset - oset), sorted(oset - nset)
    ofn, nfn = {}, {}
    for f in old["functions"]:
        ofn.setdefault(f["name"], f)
    for f in new["functions"]:
        nfn.setdefault(f["name"], f)
    added_f, removed_f = sorted(set(nfn) - set(ofn)), sorted(set(ofn) - set(nfn))
    changed_f = [n for n in sorted(set(ofn) & set(nfn)) if fn_sig(ofn[n]) != fn_sig(nfn[n])]
    print("types: +%d -%d (of %d)" % (len(added_t), len(removed_t), len(nset)))
    print("global functions: +%d -%d changed %d (of %d)" % (len(added_f), len(removed_f), len(changed_f), len(nfn)))

    def show(title, items, cap=60):
        if not items:
            return
        print("\n=== %s (%d) ===" % (title, len(items)))
        for x in items[:cap]:
            print(" ", x)
        if len(items) > cap:
            print("  ... and %d more" % (len(items) - cap))

    show("types added", added_t)
    show("types removed", removed_t)
    show("functions added", added_f)
    show("functions removed", removed_f)
    show("functions changed (params/return/flags)", changed_f)

    changed_c = []
    for n in sorted(nset & oset):
        ot, nt = old["byName"][n], new["byName"][n]
        diffs = []
        if ot.get("size") != nt.get("size"):
            diffs.append("size %s -> %s" % (ot.get("size"), nt.get("size")))
        op = {p["name"]: p for p in ot.get("props", [])}
        npr = {p["name"]: p for p in nt.get("props", [])}
        pa, pr = sorted(set(npr) - set(op)), sorted(set(op) - set(npr))
        if pa:
            diffs.append("+fields %s" % pa)
        if pr:
            diffs.append("-fields %s" % pr)
        for k in sorted(set(op) & set(npr)):
            if (op[k]["type"], op[k]["offset"]) != (npr[k]["type"], npr[k]["offset"]):
                diffs.append("field %s: %s@%s -> %s@%s" % (k, op[k]["type"], op[k]["offset"], npr[k]["type"], npr[k]["offset"]))
        om = {m["name"] for m in ot.get("methods", [])}
        nm = {m["name"] for m in nt.get("methods", [])}
        if nm - om:
            diffs.append("+methods %s" % sorted(nm - om))
        if om - nm:
            diffs.append("-methods %s" % sorted(om - nm))
        ov, nv = dict(ot.get("values", [])), dict(nt.get("values", []))
        if ov != nv:
            if set(nv) - set(ov):
                diffs.append("+values %s" % sorted(set(nv) - set(ov)))
            if set(ov) - set(nv):
                diffs.append("-values %s" % sorted(set(ov) - set(nv)))
        if diffs:
            changed_c.append((n, diffs))
    if changed_c:
        print("\n=== changed types (%d) ===" % len(changed_c))
        for n, diffs in changed_c[:80]:
            print("  %s:" % n)
            for line in diffs:
                print("      %s" % line)
        if len(changed_c) > 80:
            print("  ... and %d more" % (len(changed_c) - 80))


def main():
    global GAME
    args = sys.argv[1:]
    if "--game" in args:
        i = args.index("--game")
        GAME = args[i + 1]
        del args[i:i + 2]
    path = None
    if "--file" in args:
        i = args.index("--file")
        path = args[i + 1]
        del args[i:i + 2]
    inherited = "--all" in args
    args = [a for a in args if a != "--all"]
    if not args:
        print(__doc__)
        return
    if args[0] == "diff":
        if len(args) < 3:
            print("usage: catalog.py diff <old.json> <new.json>")
            return
        cmd_diff(load(args[1]), load(args[2]))
        return
    d = load(path or catalog_path())
    cmd, rest = args[0], args[1:]
    if cmd == "find" and rest:
        cmd_find(d, rest[0])
    elif cmd == "class" and rest:
        cmd_class(d, rest[0], inherited)
    elif cmd == "enum" and rest:
        cmd_enum(d, rest[0])
    elif cmd == "fn" and rest:
        cmd_fn(d, rest[0])
    elif cmd == "callable" and rest:
        cmd_callable(d, rest[0])
    elif cmd == "check":
        cmd_check(d)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
