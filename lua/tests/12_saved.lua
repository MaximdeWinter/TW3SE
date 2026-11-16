-- Data per save: the serializer behind w3.saved, the table per name, on_load/on_save handles.
-- tools/save_test.py saves and loads for real.

test("the serializer writes what load() reads back", function()
  local src = {n = 42, f = 0.1, neg = -7, s = "line\nwith \"quotes\"", b = true, list = {1, 2, {x = 3}},
               [5] = "five", inf = 1/0}
  local text = w3._serialize(src)
  local back = load("return " .. text, "=t", "t", {})()
  eq(back.n, 42); eq(back.f, 0.1); eq(back.neg, -7); eq(back.s, src.s); eq(back.b, true)
  eq(back.list[3].x, 3); eq(back[5], "five"); eq(back.inf, 1/0)
  eq(math.type(back.n), "integer")
end)

test("functions and game objects are left out", function()
  local p = w3.player()
  local back = load("return " .. w3._serialize({keep = 1, fn = print, obj = p}), "=t", "t", {})()
  eq(back.keep, 1); eq(back.fn, nil); eq(back.obj, nil)
end)

test("a table inside itself is an error", function()
  local t = {}; t.me = t
  local ok, err = pcall(w3._serialize, t)
  check(not ok and tostring(err):find("deeper than 64", 1, true), tostring(err))
end)

test("w3.saved gives one table per name", function()
  local a = w3.saved("test/saved")
  a.x = 1
  check(w3.saved("test/saved") == a, "the same table again")
  check(w3.saved("test/saved2") ~= a, "another name, another table")
  local ok = pcall(w3.saved)  -- the console runs as no mod
  check(not ok, "no name outside a mod is an error")
end)

test("on_load and on_save return handles, off_load and off_save take them back", function()
  local l, s = w3.on_load(function() end), w3.on_save(function() end)
  eq(type(l), "number")
  eq(type(s), "number")
  local before = w3._state()
  w3.off_load(l)
  w3.off_save(s)
  local after = w3._state()
  eq(after.loads, before.loads - 1, "loads")
  eq(after.saves, before.saves - 1, "saves")
end)
