-- Companion: a key calls a wolf that follows the player, fights beside them and stays until it is sent away (the
-- same key). The wolf is friendly and, if set, cannot die.
local wolf, h, bind

local cfg = w3.mod{
  name = "Wolf Companion", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = "A wolf that follows you and fights beside you.",
  settings = {
    {key = "key", type = "key", default = "F6", label = "Call or send away"},
    {key = "immortal", default = true, label = "Cannot die"},
    {key = "distance", type = "slider", min = 1, max = 8, default = 2.5, label = "Distance (m)"},
  },
  on_change = function(key) if key == "key" then bind() end end,
}

local function toggle()
  if wolf and w3.valid(wolf) then
    w3.destroy(wolf)
    wolf = nil
    w3.hud.message("The wolf leaves", 2)
    return
  end
  wolf = w3.spawn([[characters\npc_entities\monsters\wolf_lvl1.w2ent]], w3.ahead(3))
  if not wolf then return end
  w3.ai.companion(wolf, {immortal = cfg.immortal, distance = cfg.distance})
  w3.hud.message("A wolf joins you", 2)
end

function bind()
  if h then w3.off_key(h) end
  h = w3.on_key(cfg.key, toggle)
end
bind()
