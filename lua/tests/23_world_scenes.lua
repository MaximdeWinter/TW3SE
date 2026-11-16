-- The world (layers, ground, water, navmesh, rays) and scenes.

test("w3.world names the world, its layers and an entity's layer group", function()
  local p = w3.player(); if not p then return end
  check(w3.world.path():find("%.w2w$") ~= nil, "the world's path: " .. w3.world.path())
  local layers = w3.world.layers()
  check(#layers > 10 and layers[1] ~= "", "loaded layers: " .. #layers)
  check(#w3.world.layers(".w2l") == #layers, "the filter")
  local found = false
  for _, e in ipairs(w3.near(60)) do
    if type(w3.world.group_of(e)) == "string" then found = true break end
  end
  check(found, "a layer group of an entity near the player")
end)

test("w3.world reads the ground, the navmesh, the water and the terrain", function()
  local p = w3.player(); if not p then return end
  local q = p:GetWorldPosition()
  local g = w3.world.ground(q)
  check(g and math.abs(g - q.Z) < 3, "the ground under the player: " .. tostring(g) .. " / " .. q.Z)
  local n = w3.world.nav_z(q)
  check(n == nil or math.abs(n - q.Z) < 3, "the navmesh height: " .. tostring(n))
  local level, depth = w3.world.water(q)
  check(type(level) == "number" and type(depth) == "number", "water level and depth")
  local size, tiles = w3.world.terrain()
  check(size and size > 0 and tiles and tiles > 0, "the terrain: " .. tostring(size) .. " m, " .. tostring(tiles) .. " tiles")
end)

test("w3.world casts a ray and finds a free spot", function()
  local p = w3.player(); if not p then return end
  local q = p:GetWorldPosition()
  local hit, pos = w3.world.trace({X = q.X, Y = q.Y, Z = q.Z + 20}, {X = q.X, Y = q.Y, Z = q.Z - 50})
  check(hit and math.abs(pos.Z - q.Z) < 3, "the ray hits the ground under the player")
  local spot = w3.world.safe_spot(w3.ahead(3))
  check(spot == nil or math.abs(spot.Z - q.Z) < 5, "a free spot near the player")
  check(type(w3.world.sphere(q, 5)) == "table", "a sphere test")
end)

test("w3.scene reads whether a scene plays and refuses a missing scene", function()
  if not w3.player() then return end
  eq(w3.scene.playing(), false)
  is_error(function() w3.scene.play("no\\such\\scene.w2scene") end, "no scene")
end)
