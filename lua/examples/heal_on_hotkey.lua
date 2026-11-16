-- Press H (or LB + Y on a controller) to refill the player's health. Change the keys to any key name ("F6",
-- "Ctrl+H", "Mouse4", "Pad_RB+Pad_X").
local KEYS = {"H", "Pad_LB+Pad_Y"}

local function heal()
  local p = w3.player()
  if p then
    p:SetHealth(p:GetMaxHealth())
    print("healed")
  end
end

for _, key in ipairs(KEYS) do w3.on_key(key, heal) end
