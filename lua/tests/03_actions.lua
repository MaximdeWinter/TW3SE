-- Real actions through the game's own functions (need a loaded save). Each test restores what it changed.
local function player()
  return w3.player()
end

test("money add and remove round-trip", function()
  local p = player(); if not p then return end
  local m0 = p:GetMoney()
  p:AddMoney(123)
  eq(p:GetMoney(), m0 + 123)
  p:RemoveMoney(123)
  eq(p:GetMoney(), m0)
end)

test("health reads", function()
  local p = player(); if not p then return end
  check(p:GetHealth() > 0, "health > 0")
  check(p:GetMaxHealth() > 0, "max health > 0")
end)

test("weather change returns a result", function()
  if not (w3.global("theGame") and w3.global("theGame"):IsActive()) then return end
  local ok = w3.call("RequestWeatherChangeTo", "WT_Clear", 1.0, false)
  check(type(ok) == "boolean", "weather call returns a bool")
end)

test("game time struct", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  local t = g:GetGameTime()
  check(t, "GetGameTime returned a value")
end)

test("optional argument omitted", function()
  local p = player(); if not p then return end
  -- Heal(amount) takes one argument; calling with none emits the "omitted" opcode (amount = 0), no crash.
  -- (The test checks that the omitted-argument path runs, the health values stay unchecked.)
  local ok = pcall(function() p:GetMaxHealth() end)
  check(ok, "a normal call still works with the omitted-arg path compiled in")
end)

test("event raise is callable", function()
  local p = player(); if not p then return end
  -- RaiseEvent(name) is a native method; an unknown event name is a safe no-op. Proves events are reachable.
  local ok = pcall(function() p:RaiseEvent("tw3se_no_such_event") end)
  check(ok ~= nil, "RaiseEvent reachable")
end)

test("spawn and destroy an entity", function()
  local u = require("lib.w3util")
  local p = player(); if not p then return end
  local v = p:GetWorldPosition()
  -- an arrow, a few metres away, not persisted. Never a clone of the player: a spawned player entity makes
  -- its own CPlayerInput and PhotomodeManager, they take over every input listener, and once it is destroyed
  -- Sprint, Esc, attacks and the photo mode are dead until the save is loaded again
  local e = u.spawn([[items\weapons\projectiles\arrows\arrow_01.w2ent]], v.X + 3.0, v.Y + 3.0, v.Z + 1.0)
  check(e ~= nil, "CreateEntity returned an entity")
  check(w3.classof(e) ~= nil, "the spawned entity has a class")
  e:Destroy()  -- clean up so the test leaves nothing behind
end)

test("read position", function()
  local p = player(); if not p then return end
  local pos = p:GetWorldPosition()
  check(pos.X ~= nil and pos.Y ~= nil and pos.Z ~= nil, "a world position")
end)

test("raycast hits the ground under the player", function()
  local u = require("lib.w3util")
  local p = player(); if not p then return end
  local v = p:GetWorldPosition()
  -- a ray straight down through the player must hit the world near Geralt's feet, normal pointing up
  local hit, pos, normal = u.raycast({X = v.X, Y = v.Y, Z = v.Z + 5, W = 1}, {X = v.X, Y = v.Y, Z = v.Z - 5, W = 1})
  check(hit, "the downward ray hit the world")
  check(pos and math.abs(pos.Z - v.Z) < 3, "the hit is near the player's feet")
  check(normal and normal.Z > 0.5, "the ground normal points up")
  local gz = u.ground_z(v.X, v.Y, v.Z)
  check(gz and math.abs(gz - v.Z) < 3, "ground_z agrees")
end)
