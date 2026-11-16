"""A mod's replaced game scripts as annotations (TW3SE 1.2): no Script Merger, no conflict over a whole file.

A mod that ships its own copy of a game script (Mods\\modX\\content\\scripts\\game\\...\\file.ws) replaces the whole
file, and two such mods need Script Merger. Patch 5.0 has annotations that change one function or add one field:
@replaceMethod, @wrapMethod, @addMethod, @addField. This tool compares each replaced file with the game's own and writes
what the mod changed as annotations into one file per script:

    python tools\\to_annotations.py <mod folder> [--game <game folder>] [--out <folder>]

    <out>\\content\\scripts\\local\\<file>_annotated.ws    the changes as annotations
    <out>\\annotations_report.txt                         what the tool did and what needs a look by hand

What becomes what:
    a changed function of a class              @replaceMethod(Class) with the mod's version
    a new function of a class                  @addMethod(Class)
    a new field of a class                     @addField(Class)
    a changed global function                  @replaceMethod function ...
    a new class, struct, enum or function      copied as it is
States, events (annotations take functions only), changed defaults, changed or removed fields and removed functions
go into the report: they need a hand.
A @replaceMethod takes over the whole function; where the mod only adds lines before or after the game's code,
@wrapMethod with wrappedMethod(...) keeps other mods' changes to it. The report marks those functions.
"""
import argparse
import os
import re
import sys

ACCESS = ("private", "protected", "public")


def strip_comments(src):
    """The source with comments and string contents blanked (same length, so offsets stay)."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match_brace(clean, open_at):
    depth = 0
    for i in range(open_at, len(clean)):
        if clean[i] == "{":
            depth += 1
        elif clean[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(clean) - 1


def statement_start(clean, at):
    """Where the declaration that holds `at` begins: after the previous ; { or } (its modifiers belong to it)."""
    i = at
    while i > 0 and clean[i - 1] not in ";{}":
        i -= 1
    while i < at and clean[i] in " \t\r\n":
        i += 1
    return i


def members(src, clean, lo, hi):
    """Functions, events and fields inside [lo, hi) (a class body), and what else stands there."""
    funcs, fields, other = {}, {}, []
    i = lo
    while i < hi:
        m = re.compile(r"\b(function|event)\s+(\w+)\s*\(").search(clean, i, hi)
        v = re.compile(r"\bvar\s+([\w\s,]+?)\s*:").search(clean, i, hi)
        d = re.compile(r"\b(default|defaults|hint|autobind)\b").search(clean, i, hi)
        nxt = min([x.start() for x in (m, v, d) if x] or [hi])
        if nxt >= hi:
            break
        if m and m.start() == nxt:
            start = statement_start(clean, m.start())
            semi = clean.find(";", m.end(), hi)
            brace = clean.find("{", m.end(), hi)
            if brace < 0 or (0 <= semi < brace):  # an import function: no body
                end = semi + 1
            else:
                end = match_brace(clean, brace) + 1
            funcs[m.group(2)] = (m.group(1), src[start:end])
            i = end
        elif v and v.start() == nxt:
            start = statement_start(clean, v.start())
            end = clean.find(";", v.end(), hi) + 1
            for name in re.split(r"\s*,\s*", v.group(1).strip()):
                fields[name] = src[start:end]
            i = end
        else:
            start = statement_start(clean, d.start())
            brace = clean.find("{", d.end(), hi)
            semi = clean.find(";", d.end(), hi)
            end = match_brace(clean, brace) + 1 if 0 <= brace and (semi < 0 or brace < semi) else semi + 1
            other.append(src[start:end])
            i = end
    return funcs, fields, other


def top_level(src):
    """{key: (kind, name, text, members)}: classes, states, structs, enums, global functions, global vars."""
    clean = strip_comments(src)
    out = {}
    i = 0
    pat = re.compile(r"\b(class|struct|enum|state|function|event|var)\s+(\w+)")
    while True:
        m = pat.search(clean, i)
        if not m:
            break
        kind, name = m.group(1), m.group(2)
        start = statement_start(clean, m.start())
        if kind in ("class", "struct", "enum", "state"):
            brace = clean.find("{", m.end())
            semi = clean.find(";", m.end())
            if brace < 0 or (0 <= semi < brace):  # "import class X;" and the like
                end = semi + 1
                out[(kind, name)] = (kind, name, src[start:end], None)
            else:
                end = match_brace(clean, brace) + 1
                if kind == "state":
                    owner = re.search(r"\bin\s+(\w+)", clean[m.end():brace])
                    name = name + " in " + (owner.group(1) if owner else "?")
                body = members(src, clean, brace + 1, end - 1) if kind in ("class", "state") else None
                out[(kind, name)] = (kind, name, src[start:end], body)
            i = end
        elif kind in ("function", "event"):
            brace = clean.find("{", m.end())
            semi = clean.find(";", m.end())
            end = semi + 1 if brace < 0 or (0 <= semi < brace) else match_brace(clean, brace) + 1
            out[("function", name)] = ("function", name, src[start:end], None)
            i = end
        else:
            end = clean.find(";", m.end()) + 1
            out[("var", name)] = ("var", name, src[start:end], None)
            i = end
    return out


def norm(text):
    return re.sub(r"\s+", " ", strip_comments(text)).strip()


def without_access(text):
    """The declaration without private/protected/public (the annotation names the class)."""
    head, sep, rest = text.partition("(")
    words = [w for w in head.split() if w not in ACCESS]
    return " ".join(words) + sep + rest


def only_added_around(old, new):
    """True when the mod's body is the game's body with lines added before or after it (a @wrapMethod would do)."""
    a = [l.strip() for l in strip_comments(old).splitlines() if l.strip()]
    b = [l.strip() for l in strip_comments(new).splitlines() if l.strip()]
    if len(b) <= len(a) or len(a) < 3:
        return False
    inner_a = a[1:-1]
    for k in range(1, len(b) - len(inner_a)):
        if b[k:k + len(inner_a)] == inner_a:
            return True
    return False


def convert(mod_file, game_file, report):
    mod_src = open(mod_file, encoding="utf-8-sig", errors="replace").read()
    game_src = open(game_file, encoding="utf-8-sig", errors="replace").read()
    mine, theirs = top_level(mod_src), top_level(game_src)
    out = []
    for key, (kind, name, text, body) in mine.items():
        if key not in theirs:
            if kind == "state":
                report.append(f"  new state {name}: copied as it is (a state of a game class may need a hand)")
            out.append(text)
            continue
        gkind, _, gtext, gbody = theirs[key]
        if norm(text) == norm(gtext):
            continue
        if kind == "function":
            out.append("@replaceMethod " + without_access(text).lstrip())
            report.append(f"  global function {name}: @replaceMethod"
                          + (" (only lines added around the game's: @wrapMethod would keep other mods' changes)"
                             if only_added_around(gtext, text) else ""))
            continue
        if kind == "state":
            report.append(f"  state {name}: changed - by hand (states take no annotations here)")
            continue
        if kind != "class" or body is None or gbody is None:
            report.append(f"  {kind} {name}: changed - by hand")
            continue
        funcs, fields, other = body
        gfuncs, gfields, gother = gbody
        for fname, (fkind, ftext) in funcs.items():
            if fkind == "event" and (fname not in gfuncs or norm(ftext) != norm(gfuncs[fname][1])):
                # the compiler takes annotations on functions only ("expecting TOKEN_FUNCTION")
                report.append(f"  {name}.{fname}: event {'new' if fname not in gfuncs else 'changed'} - by hand "
                              "(annotations take functions only: move the mod's code into a function the event calls)")
                continue
            if fname not in gfuncs:
                out.append(f"@addMethod({name}) " + without_access(ftext).lstrip())
                report.append(f"  {name}.{fname}: new, @addMethod")
            elif norm(ftext) != norm(gfuncs[fname][1]):
                out.append(f"@replaceMethod({name}) " + without_access(ftext).lstrip())
                hint = " (only lines added around the game's: @wrapMethod would keep other mods' changes)" \
                    if only_added_around(gfuncs[fname][1], ftext) else ""
                report.append(f"  {name}.{fname}: changed, @replaceMethod{hint}")
        for fname in gfuncs:
            if fname not in funcs:
                report.append(f"  {name}.{fname}: removed by the mod - by hand")
        added = set()
        for vname, vtext in fields.items():
            if vname not in gfields:
                if vtext not in added:
                    out.append(f"@addField({name}) " + " ".join(w for w in vtext.split() if w not in ACCESS))
                    added.add(vtext)
                report.append(f"  {name}.{vname}: new field, @addField")
            elif norm(vtext) != norm(gfields[vname]):
                report.append(f"  {name}.{vname}: field changed - by hand")
        if [norm(o) for o in other] != [norm(o) for o in gother]:
            report.append(f"  {name}: defaults or hints changed - by hand")
    for key in theirs:
        if key not in mine:
            report.append(f"  {key[0]} {key[1]}: in the game's file, not in the mod's - by hand")
    return out


def find_game():
    """The game folder: TW3SE's repository knows it (gamesession.py), else Steam's libraries from the registry."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import gamesession
        return gamesession.GAME
    except Exception:
        pass
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            steam = winreg.QueryValueEx(k, "SteamPath")[0]
        libs = [steam]
        vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            libs += re.findall(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read())
        for lib in libs:
            g = os.path.join(lib.replace("\\\\", "\\"), "steamapps", "common", "The Witcher 3")
            if os.path.isdir(g):
                return g
    except OSError:
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mod")
    ap.add_argument("--game", default=None, help="the game folder (default: from tools\\gamesession.py)")
    ap.add_argument("--out", default=None, help="where to write (default: <mod>_annotated beside the mod)")
    a = ap.parse_args()
    game = a.game or find_game()
    if not game or not os.path.isdir(os.path.join(game, "content", "content0", "scripts")):
        sys.exit("The game folder was not found: give it with --game \"<the folder with bin and content>\"")
    vanilla = os.path.join(game, "content", "content0", "scripts")
    mod_scripts = os.path.join(a.mod, "content", "scripts")
    out_dir = a.out or a.mod.rstrip("\\/") + "_annotated"
    report = []
    written = 0
    for base, _, files in os.walk(mod_scripts):
        for f in files:
            if not f.endswith(".ws"):
                continue
            rel = os.path.relpath(os.path.join(base, f), mod_scripts)
            gfile = os.path.join(vanilla, rel)
            if not os.path.exists(gfile):
                continue  # the mod's own file (local\...): no game file it replaces
            report.append(f"{rel}:")
            pieces = convert(os.path.join(base, f), gfile, report)
            if not pieces:
                report.append("  the same as the game's (the mod's copy can go)")
                continue
            dst = os.path.join(out_dir, "content", "scripts", "local", os.path.splitext(f)[0] + "_annotated.ws")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            head = f"// {rel} of {os.path.basename(a.mod.rstrip(os.sep))} as annotations (tools\\to_annotations.py)\n\n"
            open(dst, "w", encoding="utf-8", newline="\r\n").write(head + "\n\n".join(pieces) + "\n")
            written += 1
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "annotations_report.txt"), "w", encoding="utf-8").write("\n".join(report) + "\n")
    print("\n".join(report))
    print(f"\n{written} file(s) written to {out_dir}")


if __name__ == "__main__":
    main()
