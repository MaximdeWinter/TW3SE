-- One spawned NPC and one spawned light, every check on them, both destroyed at the end.

-- A wraith: an NPC with an AI and stats of its own (a crowd NPC spawned outside a community has neither). Made
-- friendly and invulnerable at once, so it does not fight Geralt.
local NPC = "characters\\npc_entities\\monsters\\wraith_lvl1.w2ent"
local npc, lamp

test("w3.spawn puts an NPC in front of the player", function()
  local p = w3.player(); if not p then return end
  npc = w3.spawn(NPC)
  check(npc ~= nil, "spawned")
  w3.ai.set_attitude(npc, "friendly")
  w3.ai.immortal(npc, true)
  local d = w3.call("VecDistance", npc:GetWorldPosition(), p:GetWorldPosition())
  check(d > 1 and d < 6, "about 3 m away (" .. tostring(d) .. ")")
  check(#w3.all("CNewNPC") >= 1, "in the object table")
end)

test("w3.ai reads and changes attitude and attitude group", function()
  if not npc then return end
  eq(w3.ai.attitude(npc), "AIA_Friendly")
  w3.ai.set_attitude(npc, "neutral")
  eq(w3.ai.attitude(npc), "AIA_Neutral")
  w3.ai.set_attitude(npc, "friendly")
  local group = w3.ai.group(npc)
  check(type(group) == "string", "an attitude group: " .. tostring(group))
  w3.ai.set_group(npc, "friendly_to_player")
  eq(w3.ai.group(npc), "friendly_to_player")
  w3.ai.reset_group(npc)
  eq(w3.ai.group(npc), group)
  is_error(function() w3.ai.set_attitude(npc, "grumpy") end, "no value")
end)

test("w3.ai forces a behaviour tree and cancels it", function()
  if not npc then return end
  local id = w3.ai.force(npc, "CAIMoveToAction", "BTAP_Emergency", function(t)
    t.params.targetTag = "PLAYER"
    t.params.moveSpeed = 1
  end)
  check(type(id) == "number" and id >= 0, "an id: " .. tostring(id))
  check(w3.ai.cancel(npc, id), "cancelled")
end)

test("w3.stats and w3.inv work on an NPC", function()
  if not npc then return end
  eq(npc:GetImmortalityMode(), "AIM_Invulnerable")  -- (its health is set up a frame later: round2_test.py reads it)
  local n = w3.inv.count("Torch", npc)
  w3.inv.add("Torch", 1, npc)
  eq(w3.inv.count("Torch", npc), n + 1)
  check(type(w3.inv.items(npc)) == "table", "the NPC's items")
end)

test("w3.light spawns a light and changes it; w3.light.near finds it", function()
  if not w3.player() then return end
  local l
  lamp, l = w3.light.spawn(nil, {color = {255, 80, 20}, radius = 6, brightness = 30})
  check(lamp ~= nil and l ~= nil, "a lamp with a light")
  eq(l.radius, 6.0)
  eq(l.brightness, 30.0)
  eq(l.color.Red, 255)
  eq(l.color.Green, 80)
  w3.light.set(l, {on = false})
  eq(l.isEnabled, false)
  w3.light.set(l, {on = true})
  local found = false
  for _, c in ipairs(w3.light.near(10)) do if c == l then found = true end end
  check(found, "near the player")
end)

test("w3.fx plays and stops an effect on an entity", function()
  if not lamp then return end
  w3.fx.play(lamp, "fire")
  check(type(w3.fx.active(lamp, "fire")) == "boolean", "active reads")
  w3.fx.stop(lamp)
end)

test("the spawned NPC and lamp go again", function()
  if npc then w3.destroy(npc) end
  if lamp then w3.destroy(lamp) end
  npc, lamp = nil, nil
end)
