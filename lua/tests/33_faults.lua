-- Crash resistance: a fault inside a w3 function stops that one call with an ordinary Lua error; Lua and
-- the other mods go on. w3._fault reads address 0x10 or divides by zero inside the function.

test("a fault in a w3 function stops that call and Lua goes on", function()
  local ok, err = pcall(w3._fault)
  check(not ok, "the call failed")
  err = tostring(err)
  check(err:find("bad read at 0x10", 1, true) ~= nil, "the message names the read: " .. err)
  check(err:find("tw3se.dll+", 1, true) ~= nil, "the message names the place: " .. err)
  check(err:find("the game goes on", 1, true) ~= nil, "the message: " .. err)
  check(type(w3.time()) == "number", "a w3 function works after it")
end)

test("a division by zero in a w3 function is an error too", function()
  local ok, err = pcall(w3._fault, "div")
  check(not ok, "the call failed")
  check(tostring(err):find("exception 0xc0000094", 1, true) ~= nil, "the message: " .. tostring(err))
end)

test("a fault inside a coroutine ends the coroutine alone", function()
  local co = coroutine.create(function() coroutine.yield(1); w3._fault(); return 2 end)
  local a, b = coroutine.resume(co)
  check(a and b == 1, "the first part ran")
  local c, d = coroutine.resume(co)
  check(not c and tostring(d):find("bad read", 1, true) ~= nil, "the fault came back as an error: " .. tostring(d))
  eq(coroutine.status(co), "dead")
end)

test("a thousand faults in a row leave Lua as it was", function()
  local before = collectgarbage("count")
  for _ = 1, 1000 do pcall(w3._fault) end
  collectgarbage()
  check(collectgarbage("count") < before + 512, "memory: " .. before .. " -> " .. collectgarbage("count") .. " KB")
  check(type(w3.time()) == "number", "a w3 function works after them")
end)
