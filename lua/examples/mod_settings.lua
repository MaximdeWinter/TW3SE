-- A mod with settings in the TW3SE Menu (Mods > Regeneration): health comes back every second while it is on, a key
-- switches it on and off, and a tab of its own shows the player's health. Copy into lua\autorun\, or as init.lua
-- into tw3se\mods\regeneration\.
local cfg
local keyHandle

local function bind(key)  -- the key from the settings; a new one in the menu takes the old one's place
  if keyHandle then w3.off_key(keyHandle) keyHandle = nil end
  if key ~= "" then keyHandle = w3.on_key(key, function() cfg.enabled = not cfg.enabled end) end
end

cfg = w3.mod{
  name = "Regeneration", version = "1.0", author = "you",
  description = "Health comes back every second.",
  settings = {
    {key = "enabled", default = true, label = "Enabled"},
    {key = "amount", type = "slider", min = 1, max = 100, int = true, default = 10, label = "Health per second"},
    {key = "toggle", type = "key", default = "F7", label = "On and off", tip = "Switches the regeneration"},
  },
  on_change = function(key, value)
    if key == "toggle" then bind(value) end
  end,
  tab = function()
    local p = w3.player()
    if not p then w3.ui.text("no player (main menu)") return end
    w3.ui.text(string.format("Health: %.0f / %.0f", p:GetHealth(), p:GetMaxHealth()))
    w3.ui.text("Regeneration is " .. (cfg.enabled and "on" or "off"))
  end,
}
bind(cfg.toggle)

w3.every(1, function()
  local p = w3.player()
  if cfg.enabled and p then p:SetHealth(math.min(p:GetHealth() + cfg.amount, p:GetMaxHealth())) end
end)
