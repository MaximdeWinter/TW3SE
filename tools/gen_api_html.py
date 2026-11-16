r"""Writes the TW3SE Lua API as one web page with a search field, from lua\types\w3api.lua (the editor types): every
w3. function with its text, parameters and return values, in the sections of the file.

    python tools/gen_api_html.py                  -> build\api\index.html
    python tools/gen_api_html.py --out <file>
"""
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "lua", "types", "w3api.lua")


def version():
    h = open(os.path.join(ROOT, "include", "tw3se", "tw3se.h"), encoding="utf-8").read()
    return ".".join(re.search(rf"#define TW3SE_VERSION_{k}\s+(\d+)", h).group(1) for k in ("MAJOR", "MINOR", "PATCH"))


def parse():
    sections, cur = [], None
    doc, params, returns, fields = [], [], [], []
    lines = open(SRC, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        if line.startswith("-- ----") and i + 1 < len(lines) and lines[i + 1].startswith("-- "):
            cur = {"title": lines[i + 1][3:].strip(), "items": []}
            sections.append(cur)
            continue
        m = re.match(r"^---@param (\S+) (.+)$", line)  # the type may hold spaces: fun(e: table), {X: number}
        if m:
            params.append((m.group(1), m.group(2), ""))
            continue
        m = re.match(r"^---@return (.+)$", line)
        if m:
            returns.append(m.group(1))
            continue
        m = re.match(r"^---@field (\S+) (\S+)(?: (.*))?$", line)
        if m:
            fields.append((m.group(1), m.group(2), m.group(3) or ""))
            continue
        if line.startswith("---@"):
            m = re.match(r"^---@class (\S+)", line)
            if m and fields == [] and doc and cur is not None:
                pass
            continue
        if line.startswith("---"):
            doc.append(line[3:].strip())
            continue
        m = re.match(r"^function (w3[\w.]*)\((.*)\) end$", line)
        if m and cur is not None:
            cur["items"].append({"name": m.group(1), "args": m.group(2), "doc": " ".join(doc), "params": params,
                                 "returns": returns})
        doc, params, returns = [], [], []
        if not line.startswith("---"):
            fields = []
    return sections


def page(sections):
    out = []
    count = sum(len(s["items"]) for s in sections)
    for s in sections:
        if not s["items"]:
            continue
        out.append(f'<section><h2>{html.escape(s["title"])}</h2>')
        for it in s["items"]:
            sig = f'{it["name"]}({html.escape(it["args"])})'
            out.append(f'<div class="fn" data-k="{html.escape((it["name"] + " " + it["doc"]).lower())}">'
                       f'<code class="sig">{sig}</code>')
            if it["doc"]:
                out.append(f'<p>{html.escape(it["doc"])}</p>')
            if it["params"] or it["returns"]:
                out.append("<dl>")
                for n, t, _ in it["params"]:
                    out.append(f"<dt>{html.escape(n)}</dt><dd><code>{html.escape(t)}</code></dd>")
                for r in it["returns"]:
                    out.append(f"<dt>returns</dt><dd><code>{html.escape(r)}</code></dd>")
                out.append("</dl>")
            out.append("</div>")
        out.append("</section>")
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>TW3SE {version()} Lua API</title>
<style>
body {{ font: 14px/1.45 "Segoe UI", sans-serif; margin: 0; background: #1e1e20; color: #ddd; }}
header {{ position: sticky; top: 0; background: #2a2a2d; padding: 10px 24px; border-bottom: 1px solid #3a3a3e; }}
header b {{ margin-right: 16px; }}
input {{ width: 420px; padding: 6px 8px; background: #1e1e20; color: #ddd; border: 1px solid #4a4a4e; }}
main {{ padding: 8px 24px 40px; max-width: 1100px; }}
h2 {{ font-size: 16px; color: #fff; border-bottom: 1px solid #3a3a3e; padding-bottom: 4px; margin-top: 28px; }}
.fn {{ padding: 8px 0 10px; border-bottom: 1px solid #2c2c30; }}
.sig {{ font: 14px Consolas, monospace; color: #9cdcfe; }}
p {{ margin: 4px 0; }}
dl {{ display: grid; grid-template-columns: max-content auto; gap: 2px 14px; margin: 4px 0 0; color: #aaa; }}
dt {{ font-family: Consolas, monospace; }} dd {{ margin: 0; }} code {{ color: #ce9178; }}
.hide {{ display: none; }}
</style></head><body>
<header><b>TW3SE {version()} Lua API</b><input id="q" placeholder="Search {count} functions" autofocus></header>
<main>{''.join(out)}</main>
<script>
const q = document.getElementById('q');
q.addEventListener('input', () => {{
  const words = q.value.toLowerCase().split(/\\s+/).filter(Boolean);
  for (const s of document.querySelectorAll('section')) {{
    let any = false;
    for (const f of s.querySelectorAll('.fn')) {{
      const hit = words.every(w => f.dataset.k.includes(w));
      f.classList.toggle('hide', !hit);
      any = any || hit;
    }}
    s.classList.toggle('hide', !any);
  }}
}});
</script></body></html>
"""


def main():
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(ROOT, "build", "api", "index.html")
    sections = parse()
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w", encoding="utf-8", newline="\n").write(page(sections))
    n = sum(len(s["items"]) for s in sections)
    print(f"{out}: {n} functions in {len([s for s in sections if s['items']])} sections")


if __name__ == "__main__":
    sys.exit(main())
