-- Arrays grow and shrink with any element: strings, handles, structs. Each test works on an
-- object of its own (w3.new), so the game's own arrays stay as they are.

local function refs(o)
  local b = w3.peek(w3.addr(o), 8)
  return b and (w3.peek(b, 0) & 0xffffffff) or -1
end

test("an array of strings grows and shrinks", function()
  local f = w3.new("CModFilterData")
  local a = f.m_searchWords
  eq(#a, 0)
  a:insert("bear")
  a:insert("wolf")
  a:insert(1, "griffin")       -- before the first
  a[#a + 1] = "drowner"        -- appends like insert
  eq(#a, 4)
  eq(a[1], "griffin") eq(a[2], "bear") eq(a[3], "wolf") eq(a[4], "drowner")
  eq(a:remove(2), "bear")
  eq(a:remove(), "drowner")
  eq(#a, 2)
  eq(a[1], "griffin") eq(a[2], "wolf")
  a[2] = "werewolf"
  eq(a[2], "werewolf")
end)

test("many strings in and out leave nothing behind", function()
  local f = w3.new("CModFilterData")
  local a = f.m_searchWords
  for i = 1, 200 do a:insert(string.rep("x", i)) end
  eq(#a, 200)
  eq(#a[200], 200)
  for _ = 1, 200 do a:remove(1) end
  eq(#a, 0)
end)

test("an array of structs grows with a table of fields", function()
  local m = w3.new("W3GameEffectManager")
  local a = m.effectIconTypes
  a:insert({typeName = "poison", path = "icons/poison.png"})
  a:insert({typeName = "bleeding"})
  eq(#a, 2)
  eq(a[1].typeName, "poison")
  eq(a[1].path, "icons/poison.png")
  eq(a[2].typeName, "bleeding")
  eq(a[2].path, "")
  a[2] = {path = "icons/bleed.png"}
  eq(a[2].path, "icons/bleed.png")
  eq(a:remove(1), true)        -- a struct holding a string: true (its copy would point at freed memory)
  eq(#a, 1)
  eq(a[1].typeName, "bleeding")
end)

test("a wrong field in a struct leaves the array as it was", function()
  local m = w3.new("W3GameEffectManager")
  local a = m.effectIconTypes
  a:insert({typeName = "x"})
  is_error(function() a:insert({nosuchfield = 1}) end)
  eq(#a, 1)
end)

test("an array of handles holds one reference per element", function()
  local m = w3.new("W3GameEffectManager")
  local e = w3.new("CBaseGameplayEffect")
  local a = m.effects
  collectgarbage("collect")
  local before = refs(e)
  a:insert(e)
  a:insert(e)
  eq(#a, 2)
  eq(refs(e), before + 2)
  check(a[1] == e or w3.addr(a[1]) == w3.addr(e), "the element is the object")
  a:remove(1)
  collectgarbage("collect")
  eq(refs(e), before + 1)
  a:remove()
  collectgarbage("collect")
  eq(refs(e), before)
end)

test("a handle of the wrong class is refused and the array stays", function()
  local m = w3.new("W3GameEffectManager")
  local a = m.effects
  is_error(function() a:insert(w3.player()) end)
  eq(#a, 0)
end)
