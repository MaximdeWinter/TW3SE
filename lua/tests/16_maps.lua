-- Maps: a map field is a live view; the script side (TW3SE_MapHost in the test mod) sees what
-- Lua wrote and Lua sees what the script wrote.

local function host()
  local ok, h = pcall(w3.new, "TW3SE_MapHost")
  return ok and h or nil
end

test("a map field reads, adds, changes and deletes", function()
  local h = host(); if not h then return end
  local m = h.nums
  eq(#m, 0)
  m[3] = 1.5
  m[7] = 2.5
  eq(#m, 2)
  eq(m[3], 1.5)
  eq(m[7], 2.5)
  eq(m[99], nil)
  m[3] = 4.0
  eq(m[3], 4.0)
  eq(#m, 2)
  m[3] = nil
  eq(#m, 1)
  eq(m[3], nil)
  local seen = 0
  for k, v in pairs(m) do
    seen = seen + 1
    eq(k, 7)
    eq(v, 2.5)
  end
  eq(seen, 1)
end)

test("the script reads what Lua wrote, Lua reads what the script wrote", function()
  local h = host(); if not h then return end
  h.nums[5] = 0.75
  h.texts["lua"] = "from lua"
  h.names.fromLua = true
  eq(h:NumAt(5), 0.75)
  eq(h:TextAt("lua"), "from lua")
  eq(h:NameAt("fromLua"), true)
  h:FillFromScript()
  eq(h.nums[11], 0.25)
  eq(h.texts.script, "wrote this")
  eq(h.names.fromScript, true)
end)

test("string and name keys, a whole table assigned, clear", function()
  local h = host(); if not h then return end
  h.texts["a"] = "x"
  h.texts["b"] = "y"
  eq(#h.texts, 2)
  h.texts = {c = "z"}               -- exactly these entries
  eq(#h.texts, 1)
  eq(h.texts.c, "z")
  eq(h.texts.a, nil)
  local t = h.texts:to_table()
  eq(t.c, "z")
  eq(h.texts:clear(), 1)
  eq(#h.texts, 0)
  is_error(function() h.nums[1] = "not a number" end, "number")
  eq(h.nums[1], nil)                -- the refused value left no key behind
end)

test("a map grows past its first capacity", function()
  local h = host(); if not h then return end
  local m = h.nums
  for i = 1, 200 do m[i] = i * 0.5 end
  eq(#m, 200)
  eq(m[150], 75.0)
  eq(h:NumAt(200), 100.0)
  eq(#m:keys(), 200)
end)

test("the player's own maps read", function()
  local p = w3.player(); if not p then return end
  local m = p.skillComboCooldownTimerIdToSkillCounterId
  check(tostring(m):find("map<Int32", 1, true) ~= nil, tostring(m))
  check(#m >= 0, "a count")
end)
