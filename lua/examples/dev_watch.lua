-- Dev watch: the values a modder looks at most, live over the game (position, health, time, the target in combat).
-- F9 turns it on and off.
local handles = {}

local function on()
  local p = function() return w3.player() end
  handles = {
    w3.debug.watch("position", function() return p():GetWorldPosition() end),
    w3.debug.watch("health", function() local v, m = w3.stats.health() return string.format("%.0f / %.0f", v, m) end),
    w3.debug.watch("time", function() local t = w3.time.get() return string.format("day %d %02d:%02d", t.day, t.hour, t.minute) end),
    w3.debug.watch("world", function() return w3.world.path() end),
    w3.debug.watch("target", function() local t = w3.ai.target(p()) return t and t:GetDisplayName() or "-" end),
  }
end

local function off()
  for _, h in ipairs(handles) do w3.debug.unwatch(h) end
  handles = {}
end

w3.on_key("F9", function() if #handles > 0 then off() else on() end end)
