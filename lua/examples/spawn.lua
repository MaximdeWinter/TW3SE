-- Spawn a copy of the nearest NPC in front of the player (press P). A demo of entity creation.
local u = require("lib.w3util")

w3.on_key("P", function()
  local p = w3.player()
  if not p then return end
  -- never the player's template: a second player entity takes over every input listener (see lib\w3util.lua)
  local src = u.nearest("CNewNPC", 80)
  if not src then print("spawn: no NPC within 80 m") return end
  local v = p:GetWorldPosition()
  local e = u.spawn(src.template, v.X + 2.0, v.Y + 2.0, v.Z)
  print("spawn: " .. (e and w3.classof(e) or "failed"))
end)
