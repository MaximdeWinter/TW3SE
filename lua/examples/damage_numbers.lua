-- Damage numbers: every hit shows its damage over the victim for a second, rising as it fades. The player's hits
-- in yellow, the hits the player takes in red.
local cfg = w3.mod{
  name = "Damage Numbers", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = "Shows the damage of each hit over the one who takes it.",
  settings = {
    {key = "enabled", default = true, label = "Enabled"},
    {key = "seconds", type = "slider", min = 0.5, max = 3, default = 1.2, label = "Seconds on screen"},
    {key = "others", default = false, label = "Hits between NPCs too"},
  },
}

local shown = {}  -- {pos, text, colour, at}

w3.on("hit", function(e)
  if not cfg.enabled or e.damage < 1 or not e.victim then return end
  local p = w3.player()
  local mine, onMe = e.attacker == p, e.victim == p
  if not (mine or onMe or cfg.others) then return end
  local ok, pos = pcall(function() return e.victim:GetWorldPosition() end)
  if not ok then return end
  shown[#shown + 1] = {x = pos.X, y = pos.Y, z = pos.Z + 1.9, text = tostring(math.floor(e.damage + 0.5)),
                       colour = onMe and 0xFF6050 or (mine and 0xFFE070 or 0xE0E0E0), at = w3.time()}
end)

w3.on_draw(function()
  local now = w3.time()
  for i = #shown, 1, -1 do
    local s = shown[i]
    local age = (now - s.at) / cfg.seconds
    if age >= 1 then
      table.remove(shown, i)
    else
      local alpha = math.floor(255 * (1 - age))
      w3.draw.text_at({X = s.x, Y = s.y, Z = s.z + age * 0.8}, s.text, s.colour * 256 + alpha)
    end
  end
end)
