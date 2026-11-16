-- The remaining field types: bitfields with their bit names, EngineTransform, soft handles.

test("a bitfield has its bit names in w3.enum", function()
  local e = w3.enum("EEntityStaticFlags")
  eq(e.ESF_Streamed, 1)
  eq(e.ESF_NoVisibilityQuery, 2)
  eq(e.ESF_NoCompMerge, 8)
  local d = w3.enum("EDrawableFlags")
  eq(d.DF_IsVisible, 1)
  eq(d.DF_DynamicGeometry, 1 << 14)   -- after two unnamed bits (12, 13)
end)

test("a bitfield field reads as a number and writes as a number or as bit names", function()
  local p = w3.player(); if not p then return end
  local v = p.entityStaticFlags
  eq(math.type(v), "integer")
  local names = {}
  for name, bit in pairs(w3.enum("EEntityStaticFlags")) do
    if v & bit ~= 0 then names[#names + 1] = name end
  end
  p.entityStaticFlags = v               -- the same value back: nothing changes
  eq(p.entityStaticFlags, v)
  p.entityStaticFlags = names           -- the same bits by name
  eq(p.entityStaticFlags, v)
  is_error(function() p.entityStaticFlags = {"ESF_NoSuchBit"} end, "no bit")
  eq(p.entityStaticFlags, v)
end)

-- a placed object: its `transform` is where it stands (the player's stays at its spawn value, (0, 0, 0) for a player
-- at 670, 2065)
test("EngineTransform reads as position, rotation and scale", function()
  local e
  for _, o in ipairs(w3.near(40) or {}) do
    if o ~= w3.player() and w3.classof(o) == "CEntity" then e = o break end
  end
  if not e then return end
  local t = e.transform
  local w = e:GetWorldPosition()
  near(t.pos.X, w.X, "X") near(t.pos.Y, w.Y, "Y") near(t.pos.Z, w.Z, "Z")
  local dy = (t.rot.Yaw - e:GetHeading() + 540) % 360 - 180   -- the same angle: 192.8 is -167.2
  near(dy, 0, "Yaw")
  check(t.scale.X > 0, "scale " .. tostring(t.scale.X))
end)

test("a soft handle reads as the path of its resource", function()
  local r = w3.load("game\\witcher3.redgame")
  check(r, "the game resource loads")
  local path = r.defaultPlayerTemplate
  eq(type(path), "string")
  check(path:find("player.w2ent", 1, true), "the player template: " .. tostring(path))
end)

test("an EntityHandle reads as its entity, or nil", function()
  local p = w3.player(); if not p then return end
  local h = p.horseWithInventory
  local e = p:GetHorseWithInventory()
  if e then eq(w3.addr(h), w3.addr(e)) else eq(h, nil) end
end)

-- every field of the player gives a value
test("every field of the player reads as a value", function()
  local p = w3.player(); if not p then return end
  local bad = {}
  for _, f in ipairs(w3.fields(p)) do
    local name = type(f) == "table" and (f.name or f[1]) or f
    local ok, v = pcall(function() return p[name] end)
    if not ok or (type(v) == "string" and v:match("^<.+>$")) then bad[#bad + 1] = name .. "=" .. tostring(v) end
  end
  eq(#bad, 0, table.concat(bad, ", "))
end)