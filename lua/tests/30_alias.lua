-- Alias: a game file under a second path is a resource of its own, and a running mesh component takes a
-- mesh of its own (w3.deform.own). Whether the copy gets a render mesh of its own and a set on it moves this entity
-- only is for the eye: build/g6_own.lua.

local WRAITH = [[characters\npc_entities\monsters\wraith_lvl1.w2ent]]
local ALIAS = [[characters\npc_entities\monsters\wraith_lvl1__luatest.w2ent]]

test("w3.files.alias gives a game file a second path that loads as a resource of its own", function()
  eq(w3.files.alias(WRAITH, ALIAS), true)
  eq(w3.files.alias(WRAITH, ALIAS), true)  -- made before: fine
  check(w3._diskfile(ALIAS) ~= nil and w3._diskfile(ALIAS) ~= w3._diskfile(WRAITH), "a file of its own")
  local a, b = w3.load(WRAITH), w3.load(ALIAS)
  check(a and b, "both load")
  check(w3.addr(a) ~= w3.addr(b), "two resources")
  eq(w3.classof(b), "CEntityTemplate")
  eq(#b.appearances, #a.appearances)
  local _, why = w3.files.alias(WRAITH, WRAITH)
  check(why and why:find("taken"), why)
  _, why = w3.files.alias([[no\such\file.w2ent]], [[no\such\other.w2ent]])
  check(why and why:find("no game file"), why)
  _, why = w3.files.alias(WRAITH, [[tw3se_no_folder\x.w2ent]])
  check(why and why:find("no folder"), why)
end)

test("w3.deform.own gives one component a mesh of its own, release the shared one back", function()
  local p = w3.player()
  if not p then return end
  local pp = p:GetWorldPosition()
  local e = w3.spawn(WRAITH, {X = pp.X + 30, Y = pp.Y + 30, Z = pp.Z, W = 1})
  check(e, "a wraith")
  local ok, err = pcall(function()
    local c
    for _, comp in pairs(w3.deform.meshes(e)) do c = comp break end
    check(c, "a mesh component")
    local shared = c.mesh
    local path = shared:GetPath()
    local own = w3.deform.own(c)
    check(w3.addr(c.mesh) == w3.addr(own), "the component has the copy")
    check(w3.addr(own) ~= w3.addr(shared), "a mesh of its own")
    check(own:GetPath():find("__own%d+%.w2mesh$"), own:GetPath())
    eq(#own.chunks, #shared.chunks)
    check(w3.addr(w3.deform.own(c)) == w3.addr(own), "own twice: the same copy")
    eq(w3.deform.release(c), true)
    check(w3.addr(c.mesh) == w3.addr(shared), "the shared mesh back")
    eq(c.mesh:GetPath(), path)
    eq(w3.deform.release(c), false)
    local _, why = pcall(w3.set_mesh, c, e)
    check(tostring(why):find("CMesh is needed"), tostring(why))
  end)
  w3.destroy(e)
  if not ok then error(err, 0) end
end)

test("w3.attach and w3.detach need an entity with a CAppearanceComponent", function()
  if not w3.player() then return end
  local lamp = w3.light.spawn()
  check(lamp ~= nil, "a lamp")
  local ok, err = pcall(function()
    is_error(function() w3.attach(lamp, WRAITH) end, "no CAppearanceComponent")
    is_error(function() w3.attach(w3.player(), "no_such_template.w2ent") end, "no template")
  end)
  w3.destroy(lamp)
  if not ok then error(err, 0) end
end)
