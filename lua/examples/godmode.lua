-- God mode: every tick, set the player's health back to its maximum. Toggle with godmode(true/false).
local on = false

function godmode(enable)
  on = enable ~= false
  return on
end

w3.on_tick(function()
  if not on then return end
  local p = w3.player()
  if p then p:SetHealth(p:GetMaxHealth()) end
end)
