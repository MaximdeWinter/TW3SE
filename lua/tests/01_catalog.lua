-- Catalog and type reads, valid in the main menu (no save needed).

test("enum values", function()
  eq(w3.enum("EDeathType").EDT_Agony, 3)
  eq(w3.enum("EDeathType").EDT_Default, 0)
end)

test("class info", function()
  local c = w3.class("CR4Player")
  eq(c.name, "CR4Player")
  check(c.size and c.size > 0, "size")
  check(c.fields, "fields")
  check(c.methods and #c.methods > 0, "methods")
end)

test("find", function()
  check(#w3.find("weather") > 0, "weather types")
end)

test("valid", function()
  eq(w3.valid(w3.global("theGame")), true)
  eq(w3.valid(nil), false)
  eq(w3.valid("not an object"), false)
end)

test("prelude helpers exist", function()
  eq(type(w3.watch), "function")
  eq(type(w3.every), "function")
  eq(type(w3.after), "function")
  eq(type(w3.near), "function")
  eq(type(w3.dump), "function")
end)

test("dump an object's fields", function()
  local g = w3.global("theGame")
  local s = w3.dump(g)
  check(type(s) == "string" and #s > 0, "dump returns text")
  check(w3.dump(g, "cutscene"):lower():find("cutscene"), "filter works")
end)

test("methods of an object", function()
  local g = w3.global("theGame")
  local m = w3.methods(g)
  check(type(m) == "table" and #m > 0, "methods returns a list")
  check(#w3.methods(g, "time") <= #m, "filter returns a subset")
end)

test("theGame reads", function()
  local g = w3.global("theGame")
  check(g, "theGame")
  eq(w3.classof(g), "CR4Game")
  check(type(g:GetEngineTimeAsSeconds()) == "number", "engine time")
  check(type(g:GetUnixTimestamp()) == "number", "timestamp")
end)

test("global call", function()
  eq(w3.call("StrLen", "witcher"), 7)
end)

test("out-scalar parameters: StrSplitFirst fills two out strings", function()
  -- StrSplitFirst(str, divider, out left, out right) : bool - the two out strings come back as extra values,
  -- after the bool return, in parameter order. Pass placeholders ("") for the two out slots.
  local ok, left, right = w3.call("StrSplitFirst", "alpha.beta.gamma", ".", "", "")
  eq(ok, true)
  eq(left, "alpha")
  eq(right, "beta.gamma")
  -- a divider that is absent: StrSplitFirst reports false and leaves the out strings empty
  local ok2, l2 = w3.call("StrSplitFirst", "nodelimiter", ".", "", "")
  eq(ok2, false)
  eq(l2, "")
  -- the VM must stay healthy after the out-scalar calls
  eq(w3.call("StrLen", "witcher"), 7)
end)

test("out-struct parameters: RotAxes fills three vectors", function()
  -- RotAxes(rotation : EulerAngles, out forward, out right, out up): the three out Vectors come back as extra
  -- values, in parameter order. Pass placeholders ({}) for the out slots.
  local fwd, right, up = w3.call("RotAxes", {Pitch = 0, Yaw = 0, Roll = 0}, {}, {}, {})  -- void return, 3 out
  for _, v in ipairs({fwd, right, up}) do
    check(v and type(v.X) == "number" and type(v.Y) == "number" and type(v.Z) == "number", "an axis has X, Y, Z")
  end
  local function len(v) return math.sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z) end
  local function dot(a, b) return a.X * b.X + a.Y * b.Y + a.Z * b.Z end
  near(len(fwd), 1); near(len(right), 1); near(len(up), 1)                    -- the axes are unit length
  near(dot(fwd, right), 0); near(dot(fwd, up), 0); near(dot(right, up), 0)    -- and mutually perpendicular
  eq(w3.call("StrLen", "witcher"), 7)                                        -- the VM is still healthy
end)

test("hook a native global: args, call original, override", function()
  eq(w3.call("StrLen", "witcher"), 7)
  local seen = nil
  local h = w3.hook("StrLen", function(self, args, orig)
    seen = args[1]          -- the call's argument
    return orig() + 100     -- call the original and add to it
  end)
  eq(w3.call("StrLen", "witcher"), 107)
  eq(seen, "witcher")
  w3.unhook(h)
  eq(w3.call("StrLen", "witcher"), 7)
end)

test("hook replace (skip the original)", function()
  local h = w3.hook("StrLen", function(self, args, orig) return 999 end)  -- never calls orig
  eq(w3.call("StrLen", "anything"), 999)
  w3.unhook(h)
  eq(w3.call("StrLen", "anything"), 8)
end)

test("hook modifies the arguments before the original", function()
  local h = w3.hook("StrLen", function(self, args, orig)
    args[1] = "hello"       -- change the argument, then run the original on it
    return orig()
  end)
  eq(w3.call("StrLen", "x"), 5)   -- original ran on "hello", not "x"
  w3.unhook(h)
end)

test("hook observe only (original runs unchanged)", function()
  local calls = 0
  local h = w3.hook("StrLen", function(self, args, orig) calls = calls + 1 end)  -- no orig, no return
  eq(w3.call("StrLen", "witcher"), 7)   -- still the real result
  check(calls >= 1, "the hook ran")
  w3.unhook(h)
end)

test("errors are refused with a reason", function()
  is_error(function() w3.global("theGame"):IsActive(1) end, "0 argument")
  is_error(function() w3.enum("NoSuchEnum") end, "no enum")
  is_error(function() w3.call("NoSuchFunctionAtAll") end, "no global function")
end)
