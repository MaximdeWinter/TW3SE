-- The script machine: the script stack with file and line, hooks on virtual
-- methods. Needs the test mod's tw3se_test_core.ws (TW3SE_StackOuter calls TW3SE_StackInner in line 137).

test("w3.script_stack names the running script functions with file and line", function()
  local seen
  local h = w3.hook("TW3SE_StackInner", function(self, args, orig)
    seen = w3.script_stack()
    return orig()
  end)
  local r = w3.call("TW3SE_StackOuter", 5)
  w3.unhook(h)
  eq(r, 11)
  check(seen and #seen >= 2, "two frames at least: " .. tostring(seen and #seen))
  eq(seen[1].name, "TW3SE_StackInner")
  eq(seen[2].name, "TW3SE_StackOuter")
  check(seen[2].file:find("tw3se_test_core.ws", 1, true) ~= nil, "the file: " .. seen[2].file)
  eq(seen[2].line, 137)
end)

test("w3.on_line runs before a statement of a script line", function()
  local hits = {}
  local a = w3.on_line("tw3se_test_core.ws", 136, function(i) hits[#hits + 1] = i.line .. " " .. i.name end)
  local b = w3.on_line("TW3SE_Test_Core.ws", 137, function(i) hits[#hits + 1] = i.line .. " " .. i.name end)
  local c = w3.on_line("other_file.ws", 137, function(i) hits[#hits + 1] = "wrong file" end)
  w3.call("TW3SE_StackOuter", 1)
  check(w3.off_line(a) and w3.off_line(b) and w3.off_line(c), "taken off")
  w3.call("TW3SE_StackOuter", 1)
  eq(table.concat(hits, ", "), "136 TW3SE_StackOuter, 137 TW3SE_StackOuter")
  check(not w3.off_line(a), "a second off_line is refused")
end)

test("w3.on_line reads and changes the function's variables", function()
  local seen, kept
  local h = w3.on_line("tw3se_test_core.ws", 137, function(i)
    seen = i.vars.n .. " " .. i.vars.r       -- the parameter, and the local after line 136 (r = n * 2)
    i.set("r", 10)                            -- r = TW3SE_StackInner(r) runs with 10
    kept = i.set
  end)
  local r = w3.call("TW3SE_StackOuter", 1)
  w3.off_line(h)
  eq(seen, "1 2")
  eq(r, 11)
  is_error(function() kept("r", 3) end, "only inside")
end)

test("w3.script_stack is empty outside a script call", function()
  eq(#w3.script_stack(), 0)
end)

test("w3.vslot gives the function in a vtable slot", function()
  local p = w3.player(); if not p then return end
  local rva = w3.vslot(p, 140)
  check(type(rva) == "number" and rva > 0x1000, "an address in the game: " .. tostring(rva))
end)

-- CEntity's slot 140 is TeleportWithRotation (the native's handler calls [vtable+0x460], FINDINGS_X15.md)
test("w3.vhook catches a virtual method, hands on its arguments and can skip it", function()
  if not w3.player() then return end
  local lamp = w3.light.spawn()
  check(lamp ~= nil, "a lamp")
  local calls, block, arg, me = 0, false, nil, nil
  local h, h2
  local ok, err = pcall(function()               -- the hooks and the lamp go away whatever fails
  h = w3.vhook(lamp, 140, function(self, a, orig)
    calls = calls + 1
    me = self
    arg = a[1]                                   -- rdx: the address of the position
    if block then return 0 end
  end)
  check(type(h) == "number", "a handle")
  local p = lamp:GetWorldPosition()
  lamp:TeleportWithRotation({X = p.X + 1, Y = p.Y, Z = p.Z, W = 1}, lamp:GetWorldRotation())
  eq(calls, 1)
  check(me == lamp, "self is the lamp")
  check(type(arg) == "number" and arg > 0x10000, "a pointer argument: " .. tostring(arg))
  local bits = w3.peek(arg, 0) & 0xffffffff                     -- the position's X, a float
  local x = string.unpack("<f", string.pack("<I4", bits))
  check(math.abs(x - (p.X + 1)) < 0.01, "the position handed on: " .. x - p.X)
  block = true                                   -- (that a skipped teleport leaves the lamp shows frames later:
  lamp:TeleportWithRotation({X = p.X + 5, Y = p.Y, Z = p.Z, W = 1}, lamp:GetWorldRotation())  -- tools/r61_test.py)
  eq(calls, 2)
  check(w3.vunhook(h), "unhooked")
  h = nil
  lamp:TeleportWithRotation({X = p.X + 2, Y = p.Y, Z = p.Z, W = 1}, lamp:GetWorldRotation())
  eq(calls, 2)
  h2 = w3.vhook(lamp, 140, function() end)
  is_error(function() w3.vhook(lamp, 140, function() end) end, "hooked already")
  end)
  if h then w3.vunhook(h) end
  if h2 then w3.vunhook(h2) end
  w3.destroy(lamp)
  if not ok then error(err, 0) end
end)
