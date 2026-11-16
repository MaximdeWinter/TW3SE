# Shaders

TW3SE replaces shaders of the game with your own. Each shader has an id, a 16-digit hex number made from its bytes.
A file `tw3se\shaders\<id>.cso` replaces that shader in every pipeline that uses it, from the next start of the game.

## Finding a shader

Add this line to `tw3se\tw3se.config`:

```
shader_dump = on
```

At the next start every shader the game uses goes once to `tw3se\shaders\dump\<id>.<stage>.cso` (`vs`, `ps`, `cs`,
`gs`, `hs`, `ds`). `dump\pipelines.txt` lists which shaders work together in one pipeline. The game makes most of
them while it loads and the rest when new things come into view. Loading takes longer while the dump is on.

`tools\shader_find.py` lists the shaders whose bytes hold given words, for example the vertex shaders of figures (they
read the bone indices):

```
python tools\shader_find.py --stage vs BLENDINDICES
```

`dxc -dumpbin <file>` (Windows SDK) shows a shader's inputs, outputs, resources and code. Some of the game's shaders
are DXIL (`dxc`), others DXBC (`fxc`).

## Writing a replacement

The replacement needs the same inputs and outputs as the original, in the same order, and may use only the
resources the original pipeline has. Compile it the same way as the original: `dxc -T ps_6_0` for DXIL, `fxc /T ps_5_0`
(or `ps_5_1`) for DXBC. `tools\shader_magenta.py` is a worked example: it reads a shader's signature and writes a
pixel shader that paints magenta.

When the graphics driver refuses a pipeline with a replacement, TW3SE gives it the game's own shader and writes the
id to `tw3se.log`. The console command `shaders` shows how many pipelines and shaders were seen and replaced.
