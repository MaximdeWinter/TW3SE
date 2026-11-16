-- Files and resources. Loose files and CR2W: the test needs the game's wraith template with its NPC's level set
-- from 7 to 42 (tools/cr2w) in tw3se\files; TW3SE packs it at the start (Mods\mod00000_TW3SE_Files).

local WRAITH = "characters\\npc_entities\\monsters\\wraith_lvl1.w2ent"

local function loose_test_file()
  local f = io.open(w3.dir() .. "\\files\\" .. WRAITH, "rb")
  if f then f:close() return true end
  return false
end

-- A file written while the game runs loads anew. The test copies the level-42 wraith over alghoul_mh's path (that
-- alghoul spawns as a wraith for the rest of the session; the file goes away again, the next start has the game's).
test("a file changed in tw3se\\files while the game runs loads anew", function()
  if not w3.player() or not loose_test_file() then return end
  local src = io.open(w3.dir() .. "\\files\\" .. WRAITH, "rb")
  local bytes = src:read("a")
  src:close()
  local target = "characters\\npc_entities\\monsters\\alghoul_mh.w2ent"
  local out = w3.dir() .. "\\files\\" .. target
  local f = io.open(out, "wb")
  f:write(bytes)
  f:close()
  local ok, why = w3.files.reload(target)
  os.remove(out)
  check(ok, "reloaded: " .. tostring(why))
  local e = w3.spawn(target)
  local sfx, level = tostring(e:GetSfxTag()), e.level
  w3.destroy(e)
  eq(sfx, "sfx_wraith")
  eq(level, 42)
  local none, reason = w3.files.reload("characters\\no_such_file.w2ent")
  check(none == nil and reason:find("no file", 1, true), tostring(reason))
end)

-- The test wraith's compiled template has TW3SE_TickComponent (tw3se_test_core.ws) in it
test("a component of a mod's own in a template comes with every spawn and ticks", function()
  if not w3.player() or not loose_test_file() then return end
  local e = w3.spawn(WRAITH)
  local comps = e:GetComponentsByClassName("TW3SE_TickComponent")
  local c = comps and comps[1]
  local attached = c and c.attached
  w3.destroy(e)
  if not c then return end                         -- (a test file without the component)
  check(attached, "attached")
end)

test("w3.dir gives TW3SE's folder", function()
  check(w3.dir():lower():find("\\bin\\x64_dx12\\tw3se", 1, true) ~= nil, w3.dir())
end)

test("a changed game file in tw3se\\files replaces the game's file of that path", function()
  if not w3.player() or not loose_test_file() then return end   -- (only with the test file in place)
  local e = w3.spawn(WRAITH)
  check(e ~= nil, "spawned")
  local level, sfx = e.level, tostring(e:GetSfxTag())
  w3.destroy(e)
  eq(level, 42)
  eq(sfx, "sfx_wraith")
end)
