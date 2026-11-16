-- Resources made in Lua. A whole array field takes a table or another array, a pointer field takes the
-- object, w3.copy copies a template into a new one. In the game a wraith spawned from such a template was visible,
-- animated and attacked (build/g6_spawn.lua).

local WRAITH = "characters\\npc_entities\\monsters\\wraith_lvl1.w2ent"

test("a whole array field from a table and from another array", function()
  local t = w3.new("CEntityTemplate")
  t.usedAppearances = {"a", "b", "c"}
  eq(#t.usedAppearances, 3)
  eq(t.usedAppearances[2], "b")
  t.flatCompiledData = {1, 2, 3, 255}
  eq(#t.flatCompiledData, 4)
  eq(t.flatCompiledData[4], 255)
  local src = w3.load(WRAITH)
  t.flatCompiledData = src.flatCompiledData
  eq(#t.flatCompiledData, #src.flatCompiledData)
  eq(t.flatCompiledData[100], src.flatCompiledData[100])
  t.usedAppearances = {}
  eq(#t.usedAppearances, 0)
  is_error(function() t.usedAppearances = 5 end, "a table or an array")
  is_error(function() t.flatCompiledData = src.usedAppearances end, "Uint8")
end)

test("a pointer field takes the object itself", function()
  local src = w3.load(WRAITH)
  local t = w3.new("CEntityTemplate")
  t.templateParams = src.templateParams
  eq(#t.templateParams, #src.templateParams)
  check(t.templateParams[1] == src.templateParams[1], "the same object")
  is_error(function() t.entityObject = src end, "CEntity is needed")
end)

test("w3.copy fills a new template; an NPC spawns from it", function()
  if not w3.player() then return end
  local src = w3.load(WRAITH)
  local t = w3.new("CEntityTemplate")
  local skipped = w3.copy(t, src)
  check(type(skipped) == "table", "a list of skipped fields")
  eq(#t.appearances, #src.appearances)
  eq(#t.appearances[1].includedTemplates, #src.appearances[1].includedTemplates)
  local e = w3.spawn(t)
  check(e ~= nil, "spawned")
  eq(w3.classof(e), "CNewNPC")
  w3.destroy(e)
end)
