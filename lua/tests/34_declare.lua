-- Natives without an import line get a signature from w3.declare, and w3.call passes arguments and reads
-- the return through it. ExampleDouble and GetGame are natives of the game no script imports.

test("w3.declare gives a native without an import its parameters and its return", function()
  eq(w3.declare("ExampleDouble", "int -> int"), true)
  eq(w3.call("ExampleDouble", 21), 42)
  eq(w3.declare("GetGame", "-> CGame"), true)
  eq(w3.classof(w3.call("GetGame")), w3.classof(w3.global("theGame")))
end)

test("w3.declare refuses a function the scripts declare and a type it does not know", function()
  local ok, err = pcall(w3.declare, "GetWitcherPlayer", "-> CEntity")
  check(not ok and err:find("w3.call knows its parameters", 1, true), tostring(err))
  ok, err = pcall(w3.declare, "ExampleDouble", "strange -> int")
  check(not ok and err:find("unknown type 'strange'", 1, true), tostring(err))
  ok, err = pcall(w3.declare, "NoSuchNative_TW3SE", "int")
  check(not ok and err:find("no native", 1, true), tostring(err))
end)
