-- Object lifetime: a Lua object holds one reference on the game object's handle block and gives it back when
-- Lua collects it. Calls must leave the count as they found it.

local function refs(obj)
  local b = w3.peek(w3.addr(obj), 8)
  return b and (w3.peek(b, 0) & 0xffffffff) or -1
end

test("returned handles leave the count as it was", function()
  local p = w3.player(); if not p then return end
  local h = p:GetInputHandler()
  collectgarbage("collect")
  local before = refs(h)
  for _ = 1, 50 do local x = p:GetInputHandler() end
  collectgarbage("collect")
  eq(refs(h), before)
end)

test("objects as arguments leave the count as it was", function()
  local p = w3.player(); if not p then return end
  collectgarbage("collect")
  local before = refs(p)
  for _ = 1, 50 do p:GetAttitude(p) end
  collectgarbage("collect")
  eq(refs(p), before)
end)

test("a hook that reads an object argument leaves the count as it was", function()
  local p = w3.player(); if not p then return end
  local seen = 0
  local h = w3.hook("CActor", "GetAttitude", function(self, args, orig) seen = seen + 1 return orig() end)
  collectgarbage("collect")
  local before = refs(p)
  for _ = 1, 50 do p:GetAttitude(p) end
  w3.unhook(h)
  collectgarbage("collect")
  check(seen >= 50, "the hook ran")
  eq(refs(p), before)
end)

test("a held object counts once and lets go", function()
  local p = w3.player(); if not p then return end
  local h = p:GetInputHandler()
  collectgarbage("collect")
  local before = refs(h)
  local held = {}
  for i = 1, 10 do held[i] = p:GetInputHandler() end
  eq(refs(h), before + 10)
  held = nil
  collectgarbage("collect")
  eq(refs(h), before)
end)

test("an out array of objects leaves the counts as they were", function()
  local p = w3.player(); if not p then return end
  collectgarbage("collect")
  local before = refs(p)
  for _ = 1, 20 do local found = w3.call("FindGameplayEntitiesInRange", {}, p, 50, 50) end
  collectgarbage("collect")
  eq(refs(p), before)  -- the handles in the array are given back
end)

test("a call that fails on a later argument leaves the count as it was", function()
  local p = w3.player(); if not p then return end
  collectgarbage("collect")
  local before = refs(p)
  for _ = 1, 20 do pcall(w3.call, "FindGameplayEntitiesInRange", {}, p, "not a number", 1) end
  collectgarbage("collect")
  eq(refs(p), before)  -- the object argument gives back the ref taken for the call that failed
end)

test("nil leaves an optional argument out, '' is the empty name", function()
  local p = w3.player(); if not p then return end
  local plain = #w3.call("FindGameplayEntitiesInRange", {}, p, 30, 100)
  eq(#w3.call("FindGameplayEntitiesInRange", {}, p, 30, 100, nil, nil), plain, "nil for tag and flags")
  eq(#w3.call("FindGameplayEntitiesInRange", {}, p, 30, 100, "", 0), plain, "'' as the tag")
end)