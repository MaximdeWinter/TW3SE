-- Body and sound: physics, the game's sounds, animation and figures, on a spawned wraith and on a physics object
-- of the world. A world object given an impulse of 200 up rises 0.4 m and falls back; a paused game simulates
-- nothing.

local WRAITH = "characters\\npc_entities\\monsters\\wraith_lvl1.w2ent"

test("w3.figure reads and changes the appearance", function()
  if not w3.player() then return end
  local e = w3.spawn(WRAITH)
  local ok, err = pcall(function()
    local apps = w3.figure.appearances(e)
    eq(table.concat(apps, ","), "wraith_01,wraith_02,wraith_03")
    eq(w3.figure.appearance(e, "wraith_03"), "wraith_03")
  end)
  w3.destroy(e)
  if not ok then error(err, 0) end
end)

test("w3.anim sets and reads a behaviour variable; w3.sound plays a game sound", function()
  if not w3.player() then return end
  local e = w3.spawn(WRAITH)
  local ok, err = pcall(function()
    w3.anim.var(e, "tw3se_test", 0.5)
    check(type(w3.anim.var(e, "tw3se_test")) == "number", "a number back")
    w3.sound.event("gui_global_denied")
    w3.sound.event("gui_global_denied", e)
  end)
  w3.destroy(e)
  if not ok then error(err, 0) end
end)

test("w3.physics finds a physics body in the world and reads its mass", function()
  if not w3.player() then return end
  local pp = w3.player():GetWorldPosition()
  local found
  for _, c in ipairs(w3.all("CRigidMeshComponent", 3000)) do
    local ok, dyn = pcall(function() return c:HasDynamicPhysic() end)
    if ok and dyn and w3.call("VecDistance", c:GetWorldPosition(), pp) < 60 then found = c:GetEntity() break end
  end
  if not found then return end                    -- (none near this spot)
  check(#w3.physics.bodies(found) >= 1, "a body")
  check(w3.physics.mass(found) > 0, "a mass")
end)

test("w3.physics.gravity reads and sets the gravity of the world's PhysX scene", function()
  if not w3.player() then return end
  local g = w3.physics.gravity()
  check(math.abs(g.Z + 9.81) < 0.5 and math.abs(g.X) < 0.01, "the game's gravity: " .. g.X .. " " .. g.Y .. " " .. g.Z)
  local ok, err = pcall(function()
    local h = w3.physics.gravity({X = 0, Y = 0, Z = -2})
    check(math.abs(h.Z + 2) < 0.001, "set to -2: " .. h.Z)
  end)
  w3.physics.gravity(g)
  if not ok then error(err, 0) end
  check(type(w3.physics.scene()) == "number", "the scene's address")
end)

test("w3.sound.bank_loaded answers for a bank of the game", function()
  if not w3.player() then return end
  check(type(w3.sound.bank_loaded("gui.bnk")) == "boolean", "a boolean")
end)

test("w3.figure.head names Geralt's head; furs lists fur components", function()
  if not w3.player() then return end
  check(w3.figure.head() ~= nil, "a head name")
  check(type(w3.figure.furs(w3.player())) == "table", "a list")
  local e = w3.spawn(WRAITH)
  local ok, err = pcall(is_error, function() w3.figure.head(nil, e) end, "no head manager")
  w3.destroy(e)
  if not ok then error(err, 0) end
end)

test("w3.config reads and writes the engine's configuration variables", function()
  check(#w3.config_list() > 1000, "the engine's variables: " .. #w3.config_list())
  check(#w3.config_list("umbra") >= 1, "Umbra among them")
  local was = w3.config("PostProcess", "AllowVignette")
  eq(type(was), "boolean")
  eq(w3.config("PostProcess", "AllowVignette", not was), not was)
  eq(w3.config("PostProcess", "AllowVignette", was), was)
  eq(type(w3.config("Rendering", "TerrainErrorMetricMultiplier")), "number")
  is_error(function() w3.config("No", "SuchThing") end, "no engine variable")
end)

test("w3.world.sector lists the merged objects of a loaded layer", function()
  if not w3.player() then return end
  local found
  for _, path in ipairs(w3.world.layers()) do
    local ok, objs = pcall(w3.world.sector, path)
    if ok and #objs > 0 then found = objs break end
  end
  if not found then return end                    -- (no layer with merged geometry loaded here)
  local o = found[1]
  check(o.type == "mesh" or o.type == "collision" or o.type == "decal" or type(o.type) == "number", tostring(o.type))
  check(type(o.pos.X) == "number" and o.radius >= 0, "a position and a radius")
end)

test("w3.http and w3.net start requests and sockets and refuse what cannot be", function()
  is_error(function() w3.http.request({}) end, "a url is needed")
  local u = w3.net.udp(0, function() end)
  check(type(u.id) == "number", "a UDP socket")
  u:close()
  local _, why = u:send("127.0.0.1", 9, "x")
  check(why and why:find("closed"), tostring(why))
  check(type(w3.http.get("http://127.0.0.1:1/", function() end)) == "number", "a request id")
end)
