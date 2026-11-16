-- Game events: a kill heals the player a little, every item the player picks up shows on screen, and a new level
-- gives 100 crowns. Each w3.on gets a table with what belongs to the event (docs/LUA.md, "Game events").
w3.on("death", function(e)
  local p = w3.player()
  if p and e.killer == p then p:SetHealth(math.min(p:GetMaxHealth(), p:GetHealth() + 50)) end
end)

w3.on("item_added", function(e)
  if e.player and e.name then w3.hud.message(e.count .. " x " .. e.name, 2) end
end)

w3.on("level_up", function(e)
  w3.inv.add_money(100)
  w3.hud.message("Level " .. e.level .. ": 100 crowns", 3)
end)
