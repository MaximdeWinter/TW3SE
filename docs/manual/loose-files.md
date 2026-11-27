# Loose files

Put a game file into `tw3se\files\` under its depot path, and the game uses it in place of its own. TW3SE packs it
for you.

```
bin\x64_dx12\tw3se\files\characters\npc_entities\monsters\wraith_lvl1.w2ent
```

replaces `characters\npc_entities\monsters\wraith_lvl1.w2ent` of the game.

A mod brings its loose files in the `files` folder of its own folder (see "A mod's folder" in [Lua](../LUA.md)):

```
Mods\modBetterWolves\tw3se\files\characters\npc_entities\monsters\wolf_lvl1.w2ent
```

They leave with the mod. When two places have the same file, `tw3se\files\` counts first, then the mods in the
order of their names, and `tw3se.log` names the file and both places.

## At the start

At each start TW3SE packs the files into a mod of its own, `Mods\mod00000_TW3SE_Files` (`blob0.bundle` and
`metadata.store`, `texture.cache` for textures, `soundspc.cache` for sounds). Its name sorts before every other
mod, so its files win over theirs. The mod is packed again only when a file was added, changed or removed. When the
last loose file is gone, TW3SE takes the mod away.

## Textures

A DDS file under the path of a game texture replaces its pixels. The name is the texture's path with `.dds` in place
of `.xbm`:

```
bin\x64_dx12\tw3se\files\characters\models\monsters\wraith\model\t_01__wraith_d01.dds
```

replaces the pixels of `characters\models\monsters\wraith\model\t_01__wraith_d01.xbm`. TW3SE writes them into a
`texture.cache` of its mod at the start. The DDS needs the format, size and mip count of the game's texture
(`tools\texcache.py info` lists them, `tools\texcache.py dds` writes the game's texture as a DDS to start from).
Formats: DXT1, DXT3, DXT5, BC4, BC5, BC7 and uncompressed RGBA. A texture changes at the next start of the game.
TW3SE leaves out a DDS with another format, size or mip count and names the reason in `tw3se.log`.

```
python tools\texcache.py info "<game>\content\content0\texture.cache" wraith
python tools\texcache.py dds "<game>\content\content0\texture.cache" characters\models\monsters\wraith\model\t_01__wraith_d01.xbm wraith.dds
```

### Textures of your own

A DDS under a path the game has no texture for is a new texture. It needs an `.xbm` beside it, which
`tools\new_texture.py` makes from a game texture of the same format. The tool also copies the DDS beside the `.xbm`:

```
python tools\new_texture.py characters\models\main_npc\ciri\h_01_wa__ciri\h_01_wa__ciri_d01.xbm mymod\skin\head_tan_d01.xbm head_tan.dds
```

A material takes it like any texture of the game, at the next start:

```lua
w3.material.set(mi, "Diffuse", w3.load([[mymod\skin\head_tan_d01.xbm]]))
```

Its size and mip count are the DDS's own. The `.xbm` holds the key TW3SE files the pixels under (FNV-1a 32 of its
path in lower case), so it belongs to its path: for another path, run the tool again.

The `.xbm` also holds the texture's smallest mips (32 x 32 and below), taken from the DDS. The game shows them until it
has streamed the larger ones, so the texture has its own colours from the first frame. An `.xbm` made with an older
version of the tool holds the game texture's small mips; `--embed` takes them from the DDS beside it:

```
python tools\new_texture.py --embed mymod\skin\head_tan_d01.xbm
```

## Sounds

A Wwise bank (`.bnk`) or streamed sound (`.wem`) anywhere under `tw3se\files\` replaces the game's sound file of that
name, for example `tw3se\files\sound\gui.bnk`. The game keeps its sound files in one flat list, so the folder is
yours to choose. TW3SE writes them into a `soundspc.cache` of its mod at the start, and a sound changes at the next start of
the game. `tools\soundcache.py info` lists the game's sound files, `tools\soundcache.py get` writes one out.

```
python tools\soundcache.py info "<game>\content\content0\soundspc.cache" gui
python tools\soundcache.py get "<game>\content\content0\soundspc.cache" gui.bnk gui.bnk
```

## Changes while the game runs

Save a file in `tw3se\files\` while the game runs, and the game reads the new version the next time it loads the
file, for example at the next spawn of a template. TW3SE looks for changed files once a second.
What is already in the world keeps the old version. Take a file away, and the game's own version comes back the same
way. A new file comes into the game at once when its folder is one of the game's (`gameplay\items\`); in a new folder
it comes at the next start. The console command `files` does the check at once and lists what it found; `w3.files.reload(path)` does it for
one file from Lua.

At the next start the files go into the mod again.

The files go in as they are, so they have to be in the game's cooked format: a file taken from the game's bundles
and changed. Paths have to be ASCII and shorter than 256 characters; TW3SE leaves other files out and names them in
`tw3se.log`.

## Changing a game file

`tools\bundle_extract.py` copies game files out of the bundles by a path pattern, as a start for your own:

```
python tools\bundle_extract.py "gameplay\items\*potion*.xml" --game "<game folder>"
python tools\bundle_extract.py "characters\npc_entities\monsters\wolf*" --list
```

`tools\cr2w` reads files out of the game's bundles, changes them and writes them back (Python 3). An unchanged file
comes out byte for byte as it went in:

```python
import sys
sys.path.insert(0, r"C:\path\to\tw3se\tools")
from cr2w import Depot, Tree

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3"
PATH = r"characters\npc_entities\monsters\wraith_lvl1.w2ent"

t = Tree(Depot(GAME, mods=False).read(PATH))          # the game's own file
npc = next(o for o in t.objects if o.cls == "CNewNPC")
npc.set("level", 42)
open(GAME + r"\bin\x64_dx12\tw3se\files" + "\\" + PATH, "wb").write(t.to_bytes())
```

Each object has its class (`o.cls`) and its properties (`o.get(name)`, `o.set(name, value)`). An entity template
also holds a compiled copy of its entity in `flatCompiledData`, a CR2W file of its own. Change both:

```python
from cr2w import Tree
tmpl = t.objects[0]
flat = Tree(bytes(tmpl.get("flatCompiledData")))
next(o for o in flat.objects if o.cls == "CNewNPC").set("level", 42)
tmpl.set("flatCompiledData", list(flat.to_bytes()))
```

## A component of your own

A WitcherScript class that extends `CSelfUpdatingComponent` can go into a template, and every entity spawned from
it carries the component:

```
class MyTickComponent extends CSelfUpdatingComponent {
    event OnComponentAttached() { StartTicking(); }
    event OnComponentTick(dt : float) { /* each frame */ }
}
```

`tools\add_component.py` adds it to a template:

```
python tools\add_component.py tw3se\files\characters\npc_entities\monsters\wraith_lvl1.w2ent MyTickComponent
```

The class has to be in a script of an installed mod.
