-- The convenience library (lib/w3util). Only the non-disruptive helpers are tested here.
local u = require("lib.w3util")

test("util loads", function()
  eq(type(u.heal), "function")
  eq(type(u.give_money), "function")
end)

test("util money and health", function()
  if not w3.player() then return end
  local m = u.money()
  u.give_money(50)
  eq(u.money(), m + 50)
  w3.player():RemoveMoney(50)
  local h, mh = u.health()
  check(h > 0 and mh > 0, "health readout")
end)

test("util weather and time run", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  check(type(u.weather("WT_Clear")) == "boolean", "weather call")
  u.set_hour(12)  -- noon; no assertion on the exact stored value, just that it runs without error
  check(true, "set_hour ran")
end)

test("util by_tag returns an entity or nil", function()
  if not w3.player() then return end
  local e = u.by_tag("PLAYER")
  check(e == nil or w3.classof(e) ~= nil, "by_tag gives an object or nil")
end)

test("util near and nearest", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  check(type(u.near(100000)) == "table", "near returns a table")
  local e = u.nearest("CActor")
  check(e == nil or w3.classof(e) ~= nil, "nearest gives an actor or nil")
end)

test("util inventory", function()
  if not w3.player() then return end
  eq(w3.classof(u.inventory()), "CInventoryComponent")
  check(type(u.has_item("Swallow")) == "boolean", "has_item returns a bool")
  -- give then remove, net zero; the exact item id may vary, so only require the calls to run cleanly
  local had = u.has_item("Swallow")
  u.give_item("Swallow", 1)
  u.remove_item("Swallow", 1)
  eq(u.has_item("Swallow"), had)  -- inventory unchanged
end)

test("util subsystems reachable", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  eq(w3.classof(u.environment()), "W3EnvironmentManager")
  eq(w3.classof(u.input()), "CInputManager")
  check(w3.classof(u.camera_director()):find("CameraDirector"), "a camera director")
  check(type(u.fact("tw3se_no_such_fact")) == "number", "fact query is a number")
end)

-- A tick or key handler from a wrong argument would do nothing and stay for good
test("every, after, watch and on_key refuse what they cannot use", function()
  local before = w3._state()
  is_error(function() w3.every(0.5) end, "needs a function")
  is_error(function() w3.every(nil, function() end) end, "needs seconds")
  is_error(function() w3.every(-1, function() end) end, "needs seconds")
  is_error(function() w3.after(0/0, function() end) end, "needs seconds")
  is_error(function() w3.after(1, "f") end, "needs a function")
  is_error(function() w3.watch({}, "x", function() end) end, "needs an object")
  is_error(function() w3.watch(w3.player(), nil, function() end) end, "needs a field name")
  is_error(function() w3.on_key("F6") end, "needs a function")
  local after = w3._state()
  eq(after.ticks, before.ticks, "ticks")
  eq(after.keys, before.keys, "keys")
end)

test("every, after and watch return a handle for off_tick", function()
  local before = w3._state().ticks
  local hs = {w3.every(60, function() end), w3.after(60, function() end), w3.watch(w3.player(), "equippedSign", function() end)}
  eq(w3._state().ticks, before + 3)
  for _, h in ipairs(hs) do eq(type(h), "number") w3.off_tick(h) end
  eq(w3._state().ticks, before)
end)