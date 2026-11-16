-- Hook chains: several handlers on one function, by priority; w3.unhook takes one out and leaves the others.

test("a chain on a native runs every handler, highest priority first", function()
  local base = w3.call("StrLen", "witcher")                                -- 7
  local a = w3.hook("StrLen", function(self, args, orig) return orig() + 1 end)         -- inner
  local b = w3.hook("StrLen", function(self, args, orig) return orig() * 10 end, 5)     -- outer
  local both = w3.call("StrLen", "witcher")
  check(w3.unhook(b), "unhook b")
  local inner = w3.call("StrLen", "witcher")
  check(w3.unhook(a), "unhook a")
  eq(both, (base + 1) * 10)
  eq(inner, base + 1)
  eq(w3.call("StrLen", "witcher"), base)
  check(not w3.unhook(a), "a second unhook finds nothing")
end)

test("a chain on a WitcherScript method", function()
  local p = w3.player(); if not p then return end
  local base = p:GetHealth()
  local a = w3.hook("CActor", "GetHealth", function(self, args, orig) return orig() + 1 end)
  local b = w3.hook("CActor", "GetHealth", function(self, args, orig) return orig() + 100 end, -1)  -- innermost
  local both = p:GetHealth()
  w3.unhook(a)
  local one = p:GetHealth()
  w3.unhook(b)
  near(both, base + 101)
  near(one, base + 100)
  near(p:GetHealth(), base)
end)

test("an observer sees what the handler below returned", function()
  local seen
  local r = w3.hook("StrLen", function(self, args, orig) return 42 end)                         -- replaces
  local o = w3.hook("StrLen", function(self, args, orig) seen = orig() end, 1)                  -- observes
  local got = w3.call("StrLen", "abc")
  w3.unhook(o)
  w3.unhook(r)
  eq(seen, 42)
  eq(got, 42)
end)

test("a handler with an error passes the call on", function()
  local base = w3.call("StrLen", "abcd")
  local bad = w3.hook("StrLen", function(self, args, orig) error("on purpose") end, 2)
  local good = w3.hook("StrLen", function(self, args, orig) return orig() + 1 end)
  local got = w3.call("StrLen", "abcd")
  w3.unhook(bad)
  w3.unhook(good)
  eq(got, base + 1)
end)

test("a handler that changes the arguments", function()
  local h = w3.hook("StrLen", function(self, args, orig) args[1] = args[1] .. "xx" return orig() end)
  local got = w3.call("StrLen", "abc")
  w3.unhook(h)
  eq(got, 5)
end)
