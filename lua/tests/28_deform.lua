-- Blend shapes: w3.deform on one of the player's meshes (loaded at all times; a fresh spawn's render data
-- comes a few frames later). In the game the shapes showed as planned on a wraith (inflate, twist, weights every
-- tick, 192 shapes on six meshes; build/bs_test.py); here the API and the file checks.

local function twdf(path, names, chunks)  -- chunks = {{index, n, deltas (flat list of K * n * 3 numbers)}}
  local parts = {string.pack("<c4I4I4I4", "TWDF", 1, #names, #chunks)}
  for _, name in ipairs(names) do parts[#parts + 1] = string.pack("<s2", name) end
  for _, c in ipairs(chunks) do
    parts[#parts + 1] = string.pack("<I4I4", c[1], c[2])
    for _, v in ipairs(c[3]) do parts[#parts + 1] = string.pack("<f", v) end
  end
  local f = assert(io.open(w3.dir() .. "\\" .. path, "wb"))
  f:write(table.concat(parts))
  f:close()
end

test("w3.deform loads, weights and unloads a shape set, and checks the file", function()
  if not w3.player() then return end
  local head
  for _, c in pairs(w3.deform.meshes(w3.player())) do head = c end
  check(head ~= nil, "a mesh of the player")
  local ok, err = pcall(function()
    local id, why = w3.deform.load(head, "luatest_missing.twdf")
    eq(id, nil)
    eq(why, "cannot read the file")
    twdf("luatest_deform.twdf", {"a", "b"}, {})
    local set = assert(w3.deform.load(head, "luatest_deform.twdf"))
    eq(table.concat(w3.deform.names(set), ","), "a,b")
    check(w3.deform.state(set) == "loading" or w3.deform.state(set) == "ready", w3.deform.state(set))
    eq(w3.deform.set(set, "b", 0.5), true)
    eq(w3.deform.set(set, 1, 0.25), true)
    eq(w3.deform.set(set, "nope", 1), false)
    eq(w3.deform.set(set, 3, 1), false)
    local again, why2 = w3.deform.load(head, "luatest_deform.twdf")
    eq(again, nil)
    check(why2:find("has a set already"), why2)
    eq(w3.deform.unload(set), true)
    twdf("luatest_deform.twdf", {"a"}, {{0, 1, {0, 0, 0}}})
    local bad, why3 = w3.deform.load(head, "luatest_deform.twdf")
    eq(bad, nil)
    check(why3:find("chunk 0 has %d+ vertices, the file 1"), why3)
    eq(w3.deform.state(123456), "none")
  end)
  os.remove(w3.dir() .. "\\luatest_deform.twdf")
  if not ok then error(err, 0) end
end)
