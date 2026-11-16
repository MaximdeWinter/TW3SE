-- Rest anywhere: a key asks in the game's own window whether to wait some hours, outside combat only.
local h, bind  -- the key's handle; bind() puts the key on (again after it changed)

local cfg = w3.mod{
  name = "Rest Anywhere", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = "Wait some hours anywhere, with a key.",
  settings = {
    {key = "key", type = "key", default = "F8", label = "Key"},
    {key = "hours", type = "slider", min = 1, max = 12, int = true, default = 4, label = "Hours"},
  },
  on_change = function(key) if key == "key" then bind() end end,
}

local function ask()
  local p = w3.player()
  if not p or w3.ai.in_combat(p) then return end
  w3.hud.ask("Rest for " .. cfg.hours .. " hours?", function(yes)
    if yes then w3.time.add(cfg.hours) end
  end, {title = "Rest"})
end

function bind()
  if h then w3.off_key(h) end
  h = w3.on_key(cfg.key, ask)
end
bind()
