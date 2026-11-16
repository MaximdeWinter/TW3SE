-- Bounty: crowns for every enemy the player kills, more for a higher level. The total stays across every save.
local cfg = w3.mod{
  name = "Bounty", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = "Crowns for every kill, by the enemy's level.",
  settings = {
    {key = "per_level", type = "slider", min = 0, max = 20, int = true, default = 3, label = "Crowns per level"},
    {key = "message", default = true, label = "Show a message"},
  },
  tab = function() w3.ui.text("Crowns earned in all games: " .. (w3.store().total or 0)) end,
}
local store = w3.store()

w3.on("death", function(e)
  if e.killer ~= w3.player() or e.victim == w3.player() then return end
  local ok, level = pcall(w3.stats.level, e.victim)
  local crowns = math.max(1, (ok and level or 1) * cfg.per_level)
  w3.inv.add_money(crowns)
  store.total = (store.total or 0) + crowns
  if cfg.message then w3.hud.message("Bounty: " .. crowns .. " crowns", 2) end
end)
