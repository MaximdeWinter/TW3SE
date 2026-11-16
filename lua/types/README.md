# Editor types

`w3api.lua` describes the TW3SE Lua API (`w3.*`). `w3.lua` describes the game's classes, enums and global
functions. It is generated from the RTTI catalog and `.gitignore` leaves it out (it belongs to one game build).

The release puts `w3api.lua` into `bin\x64_dx12\tw3se\lua\types`, the SDK both files into `types`. A mod folder
`Mods\<modName>\tw3se` finds them through the `.luarc.json` of the template (`lua\template`, see
`docs\manual\first-lua-mod.md`).

Generate `w3.lua`:

```
python tools/gen_types.py                      reads _cache\catalog\rtti.json, writes lua\types\w3.lua
python tools/gen_types.py --game "<game>"      reads and writes in the game's bin\x64_dx12\tw3se
```

`rtti.json` comes from the console command `catalog` (`tools/catalog_run.py` keeps a copy in `_cache\catalog\`).
