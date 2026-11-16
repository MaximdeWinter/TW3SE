-- Time, weather, environment, camera, stats, inventory and horse, all at the player.
-- Every test puts back what it changed.

test("w3.time reads, sets and adds game time; w3.time() is still the clock", function()
  if not w3.player() then return end
  check(type(w3.time()) == "number", "the real clock")
  local t = w3.time.get()
  check(t.hour >= 0 and t.hour < 24 and t.minute >= 0 and t.minute < 60, "hour and minute")
  local h = (t.hour + 3) % 24
  w3.time.set(h, 15)
  local n = w3.time.get()
  eq(n.hour, h)
  eq(n.minute, 15)
  w3.time.add(1, 30)
  eq(w3.time.get().hour, (h + 1 + (15 + 30 >= 60 and 1 or 0)) % 24)
  w3.time.set(t.hour, t.minute, t.day)
  eq(w3.time.get().hour, t.hour)
end)

test("w3.time.speed reads and sets the game's pace", function()
  if not w3.player() then return end
  local s = w3.time.speed()
  check(s > 0, "a pace")
  w3.time.speed(s * 2)
  check(math.abs(w3.time.speed() - s * 2) < 1e-4, "doubled")
  w3.time.speed(s)
  check(math.abs(w3.time.speed() - s) < 1e-4, "back")
end)

test("w3.weather and w3.env list and read", function()
  if not w3.player() then return end
  local names = w3.weather.names()
  check(#names > 0, "weather names")
  check(type(w3.weather.get()) == "string", "the current weather")
  check(type(w3.weather.rain()) == "number" and type(w3.weather.snow()) == "number", "rain and snow")
  check(#w3.env.all() > 0, "environment definitions")
  check(type(w3.env.active()) == "table", "the active ones")
end)

test("w3.camera reads position, direction and field of view; the free camera turns on and off", function()
  local p = w3.player(); if not p then return end
  local d = w3.call("VecDistance", w3.camera.pos(), p:GetWorldPosition())
  check(d < 30, "the camera is near the player (" .. tostring(d) .. " m)")
  check(w3.camera.fov() > 10, "a field of view")
  check(type(w3.camera.heading()) == "number", "a heading")
  local f = w3.camera.forward()
  check(math.abs(f.X * f.X + f.Y * f.Y + f.Z * f.Z - 1) < 0.05, "forward is a unit vector")
  local c = w3.camera.pos()
  local x, y = w3.camera.to_screen({X = c.X + f.X * 10, Y = c.Y + f.Y * 10, Z = c.Z + f.Z * 10, W = 1})
  check(x and math.abs(x - 0.5) < 0.05 and math.abs(y - 0.5) < 0.05, "a point ahead is in the middle of the screen")
  eq(w3.camera.free(), false)
  eq(w3.camera.free(true), true)
  eq(w3.camera.free(false), false)
end)

test("w3.stats reads and sets stats, abilities and buffs", function()
  local p = w3.player(); if not p then return end
  local v, vmax = w3.stats.get("vitality")
  check(vmax > 0 and v <= vmax, "vitality and its maximum")
  local hp, hpmax = w3.stats.health()
  check(hpmax > 0, "health")
  local tox = w3.stats.get("toxicity")
  eq(math.floor(w3.stats.set("toxicity", tox + 5) + 0.5), math.floor(tox + 5 + 0.5))
  w3.stats.set("toxicity", tox)
  local ab = w3.stats.abilities()
  check(#ab > 0, "abilities")
  check(w3.stats.has_ability(ab[1]), "has its first ability")
  check(w3.stats.level() >= 1, "a level")
  local had = w3.stats.has_buff("EET_WellFed")
  w3.stats.buff("wellfed")
  check(w3.stats.has_buff("EET_WellFed"), "the buff is on")
  local found = false
  for _, b in ipairs(w3.stats.buffs()) do if b == "EET_WellFed" then found = true end end
  check(found, "the buff is in the list")
  if not had then w3.stats.unbuff("wellfed") eq(w3.stats.has_buff("EET_WellFed"), false) end
  is_error(function() w3.stats.get("nosuchstat") end, "no value")
end)

test("w3.inv adds, counts and removes items and money", function()
  local p = w3.player(); if not p then return end
  local before = w3.inv.count("Torch")
  w3.inv.add("Torch", 2)
  eq(w3.inv.count("Torch"), before + 2)
  w3.inv.remove("Torch", 2)
  eq(w3.inv.count("Torch"), before)
  local m = w3.inv.money()
  eq(w3.inv.add_money(7), m + 7)
  eq(w3.inv.remove_money(7), m)
  local items = w3.inv.items()
  check(#items > 0 and type(items[1].name) == "string" and items[1].count >= 1, "the item list")
  check(w3.inv.price(items[1].id) >= 0, "a price")
end)

test("w3.horse finds the horse manager and Roach's gear", function()
  local p = w3.player(); if not p then return end
  check(w3.horse.manager() ~= nil, "the horse manager")
  check(type(w3.horse.items()) == "table", "Roach's items")
  check(w3.vehicle() == nil or w3.classof(w3.vehicle()) ~= nil, "the vehicle in use")
end)
