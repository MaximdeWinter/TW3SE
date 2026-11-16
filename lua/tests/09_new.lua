-- w3.new: objects of scripted classes, as `new <Class> in <parent>` makes them.

local function refs(obj)
  local b = w3.peek(w3.addr(obj), 8)
  return b and (w3.peek(b, 0) & 0xffffffff) or -1
end

test("w3.new makes an object with its defaults", function()
  local o = w3.new("W3NotificationData")
  eq(w3.classof(o), "W3NotificationData")
  check(w3.valid(o), "valid")
end)

test("w3.new with a parent", function()
  local p = w3.player(); if not p then return end
  local o = w3.new("W3NotificationData", p)
  eq(w3.classof(o), "W3NotificationData")
end)

test("a new object's fields can be written and read", function()
  local o = w3.new("W3MessagePopupData")
  local f = w3.fields(o)
  local name
  for n, info in pairs(f) do
    if info.type == "String" then name = n break end
  end
  if not name then return end
  o[name] = "hello"
  eq(o[name], "hello")
end)

test("a held new object counts its own ref and Lua's", function()
  local o = w3.new("W3NotificationData")
  collectgarbage("collect")
  eq(refs(o), 2)
end)

test("a garbage collection in the middle of calls leaves the calls alone", function()
  -- A collection can release objects from w3.new while a call's arguments are being built. Their discard (engine
  -- code) waits for the next tick, so the calls in between all succeed.
  local p = w3.player(); if not p then return end
  for _ = 1, 200 do w3.new("W3NotificationData") end
  local pos, failed = p:GetWorldPosition(), 0
  for i = 1, 600 do
    if i % 3 == 0 then collectgarbage("step", 40) end
    local ok = pcall(function() return w3.call("VecDistance", pos, p:GetWorldPosition()) end)
    if not ok then failed = failed + 1 end
  end
  eq(failed, 0)
end)

test("w3.new refuses what is no class", function()
  is_error(function() w3.new("NoSuchClassAtAll") end, "no class")
end)

test("w3.defaults gives a class's default values", function()
  local d = w3.defaults("W3MessagePopupData")
  eq(d.progress, -1)                         -- `default progress = -1;` in messagePopup.ws
  eq(d.titleText, "")
  is_error(function() w3.defaults("NoSuchClass_TW3SE") end, "no class")
end)

test("w3.discard lets a w3.new object go and refuses the game's own", function()
  local o = w3.new("W3NotificationData")
  eq(w3.discard(o), true)
  local ok, err = pcall(w3.discard, w3.global("theGame"))
  check(not ok and tostring(err):find("only an object w3.new made", 1, true), tostring(err))
end)
