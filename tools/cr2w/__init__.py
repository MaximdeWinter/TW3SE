"""CR2W files of The Witcher 3: read them from the game's bundles, change them, write them back.

    from cr2w import Depot, Tree
    d = Depot(r"Q:\\...\\The Witcher 3", mods=False)      # the game's own files
    t = Tree(d.read(r"characters\\npc_entities\\monsters\\wraith_lvl1.w2ent"))
    t.objects[1].set("level", 42)
    open(r"...\\bin\\x64_dx12\\tw3se\\files\\characters\\npc_entities\\monsters\\wraith_lvl1.w2ent", "wb").write(t.to_bytes())

A file written to tw3se\\files\\ under its depot path replaces the game's file at the next start.
"""
from .bundles import Depot, entries, read
from .cr2w import CR2W
from .tree import Tree, Prop, Struct, Raw, Wide, Loc, Soft, Guid, Tags, Transform, Variant
