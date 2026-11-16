"""A browsable catalog of the game's types as a static web page, from the rtti.json the console command
`catalog` writes: every class with its parents, children, fields and methods, every enum with its values, every
global function, and a search over all of them. One folder, no server: open index.html, or put it on GitHub Pages.

    python tools/catalog_html.py --game "C:\\...\\The Witcher 3"      (or --file rtti.json, or TW3SE_GAME)
    -> build\\catalog_site\\index.html + catalog.js
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "catalog_site")


def main():
    args = sys.argv[1:]
    path = None
    if "--game" in args:
        catalog.GAME = args[args.index("--game") + 1]
    if "--file" in args:
        path = args[args.index("--file") + 1]
    d = catalog.load(path or catalog.catalog_path())
    classes, enums = {}, {}
    children = {}
    for t in d["types"]:
        if t["kind"] == 2:
            if t.get("parent"):
                children.setdefault(t["parent"], []).append(t["name"])
            classes[t["name"]] = {
                "p": t.get("parent", ""),
                "s": t.get("size", 0),
                "f": [[p["name"], catalog.ws_type(p["type"]), p.get("offset", 0), " ".join(catalog.prop_mods(p.get("flags", 0)))]
                      for p in t.get("props", [])],
                "m": [[m["name"], catalog.signature(m), 1 if m["flags"] >> 6 & 1 else 0] for m in t.get("methods", [])],
            }
        elif t["kind"] in (1, 9):
            enums[t["name"]] = {"s": t.get("size", 0), "v": t.get("values", []), "b": t["kind"] == 9}
    for name, kids in children.items():
        if name in classes:
            classes[name]["c"] = sorted(kids)
    funcs = [[f["name"], catalog.signature(f), 1 if f["flags"] >> 6 & 1 else 0] for f in d["functions"]]
    data = {"build": d.get("build", ""), "classes": classes, "enums": enums, "funcs": funcs}
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "catalog.js"), "w", encoding="utf-8") as f:
        f.write("window.CATALOG = ")
        json.dump(data, f, separators=(",", ":"))
        f.write(";\n")
    with open(os.path.join(OUT, "index.html"), "w", encoding="utf-8") as f:
        f.write(PAGE)
    n_methods = sum(len(c["m"]) for c in classes.values())
    print(f"{OUT}: {len(classes)} classes, {n_methods} methods, {len(enums)} enums, {len(funcs)} global functions")


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>The Witcher 3 catalog (TW3SE)</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font: 14px/1.45 "Segoe UI", sans-serif; margin: 0; background: #16181d; color: #dde1e8; }
header { position: sticky; top: 0; background: #1f2229; padding: 10px 16px; border-bottom: 1px solid #333; z-index: 1; }
header input { width: min(560px, 90vw); padding: 7px 10px; font-size: 15px; background: #111; color: #eee; border: 1px solid #444; border-radius: 4px; }
header span { color: #8a93a3; margin-left: 12px; }
main { padding: 12px 16px 60px; max-width: 1200px; }
a { color: #7fb4ff; text-decoration: none; } a:hover { text-decoration: underline; }
h1 { font-size: 22px; margin: 6px 0 2px; } h2 { font-size: 16px; margin: 22px 0 6px; color: #b9c2d0; }
.muted { color: #8a93a3; } .lat { color: #e0a060; font-size: 12px; }
table { border-collapse: collapse; width: 100%; } td, th { padding: 3px 8px; border-bottom: 1px solid #2a2e36; text-align: left; vertical-align: top; }
th { color: #8a93a3; font-weight: normal; } code, .sig { font-family: Consolas, monospace; font-size: 13px; }
.hit { padding: 2px 0; } .kind { display: inline-block; width: 64px; color: #8a93a3; }
details summary { cursor: pointer; color: #b9c2d0; margin: 14px 0 4px; }
</style></head><body>
<header><input id="q" placeholder="Search classes, enums, functions, methods" autofocus><span id="info"></span></header>
<main id="main"></main>
<script src="catalog.js"></script>
<script>
const C = window.CATALOG, main = document.getElementById("main"), q = document.getElementById("q");
document.getElementById("info").textContent = Object.keys(C.classes).length + " classes, " + Object.keys(C.enums).length +
  " enums, " + C.funcs.length + " global functions" + (C.build ? " - game build " + C.build : "");
const esc = s => String(s).replace(/[&<>]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;"}[c]));
// a type name with every class or enum in it as a link
const typeLinks = s => esc(s).replace(/[A-Za-z_][A-Za-z0-9_]*/g, w => C.classes[w] ? `<a href="#c/${w}">${w}</a>` : C.enums[w] ? `<a href="#e/${w}">${w}</a>` : w);
function chain(name) { const out = []; while (name && C.classes[name] && out.length < 64) { out.push(name); name = C.classes[name].p; } return out; }
function showClass(name) {
  const c = C.classes[name]; if (!c) return showSearch("");
  const ch = chain(name);
  let h = `<h1>${esc(name)}</h1><div class="muted">size ${c.s} bytes</div><p>${ch.map(n => n === name ? esc(n) : `<a href="#c/${n}">${n}</a>`).join(" &rarr; ")}</p>`;
  if (c.c) h += `<h2>Classes derived from it (${c.c.length})</h2><p>${c.c.map(n => `<a href="#c/${n}">${n}</a>`).join(", ")}</p>`;
  ch.forEach((n, i) => {
    const k = C.classes[n];
    const body = (k.f.length ? `<table><tr><th>Field</th><th>Type</th><th>Offset</th><th>Modifiers</th></tr>` +
      k.f.map(f => `<tr><td><code>${esc(f[0])}</code></td><td class="sig">${typeLinks(f[1])}</td><td>${f[2]}</td><td class="muted">${esc(f[3])}</td></tr>`).join("") + `</table>` : "") +
      (k.m.length ? `<table style="margin-top:8px"><tr><th>Method</th></tr>` +
      k.m.map(m => `<tr><td class="sig">${typeLinks(m[1])}${m[2] ? ' <span class="lat">latent</span>' : ""}</td></tr>`).join("") + `</table>` : "");
    if (!body) return;
    h += i === 0 ? `<h2>Fields and methods</h2>${body}` : `<details><summary>From ${n} (${k.f.length} fields, ${k.m.length} methods)</summary>${body}</details>`;
  });
  main.innerHTML = h; window.scrollTo(0, 0);
}
function showEnum(name) {
  const e = C.enums[name]; if (!e) return showSearch("");
  main.innerHTML = `<h1>${esc(name)}</h1><div class="muted">${e.b ? "bitfield" : "enum"}, ${e.s} byte(s)</div>` +
    (e.v.length ? `<table><tr><th>Value</th><th>Number</th></tr>${e.v.map(v => `<tr><td><code>${esc(v[0])}</code></td><td>${v[1]}</td></tr>`).join("")}</table>` : `<p class="muted">No value names in the catalog.</p>`);
  window.scrollTo(0, 0);
}
function showSearch(text) {
  const t = text.trim().toLowerCase();
  if (!t) { main.innerHTML = `<p>Type a name above: a class (<a href="#c/CR4Player">CR4Player</a>), an enum (<a href="#e/EDeathType">EDeathType</a>), a function or a method.</p><p class="muted">From the game's own type information, written by TW3SE's console command <code>catalog</code>.</p>`; return; }
  const hits = [];
  for (const n in C.classes) if (n.toLowerCase().includes(t)) hits.push([0, `<span class="kind">class</span><a href="#c/${n}">${n}</a>`]);
  for (const n in C.enums) if (n.toLowerCase().includes(t)) hits.push([1, `<span class="kind">enum</span><a href="#e/${n}">${n}</a>`]);
  for (const f of C.funcs) if (f[0].toLowerCase().includes(t)) hits.push([2, `<span class="kind">global</span><span class="sig">${typeLinks(f[1])}</span>`]);
  for (const n in C.classes) for (const m of C.classes[n].m) if (m[0].toLowerCase().includes(t))
    hits.push([3, `<span class="kind">method</span><a href="#c/${n}">${n}</a>.<span class="sig">${typeLinks(m[1])}</span>`]);
  hits.sort((a, b) => a[0] - b[0]);
  main.innerHTML = `<p class="muted">${hits.length} hits${hits.length > 400 ? ", the first 400 shown" : ""}</p>` + hits.slice(0, 400).map(h => `<div class="hit">${h[1]}</div>`).join("");
}
function route() {
  const h = decodeURIComponent(location.hash.slice(1));
  if (h.startsWith("c/")) showClass(h.slice(2)); else if (h.startsWith("e/")) showEnum(h.slice(2)); else showSearch(q.value);
}
let timer; q.addEventListener("input", () => { clearTimeout(timer); timer = setTimeout(() => { if (location.hash) history.pushState(null, "", " "); showSearch(q.value); }, 150); });
window.addEventListener("hashchange", route); route();
</script></body></html>
"""

if __name__ == "__main__":
    main()
