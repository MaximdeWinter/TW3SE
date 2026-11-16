-- My Mod: heals the player on a key. A template: rename the folder modMyMod, change the name below, write your mod.
-- The file runs when the game starts. Save it while the game runs and it loads again within a second.

local saved = w3.saved()   -- this mod's data in the current save
local cfg                  -- its settings (w3.mod below)
local h                    -- the key's handle

local function heal()
  local v, max = w3.stats.get("vitality")
  w3.stats.set("vitality", math.min(max, v + max * cfg.amount / 100))
  saved.heals = (saved.heals or 0) + 1
  w3.hud.message("Healed (" .. saved.heals .. " times in this save)", 2)
end

cfg = w3.mod{
  name = "My Mod", version = "1.0", author = "you",
  tw3se = "1.2.0",   -- the TW3SE version the mod needs
  description = "Heals the player on a key.",
  settings = {
    {key = "amount", type = "slider", min = 10, max = 100, int = true, default = 50, label = "Heal (percent)"},
    {key = "key", type = "key", default = "F7", label = "Key"},
  },
  on_change = function(key, value)
    if key == "key" then
      w3.off_key(h)
      h = w3.on_key(value, heal)
    end
  end,
}

h = w3.on_key(cfg.key, heal)
