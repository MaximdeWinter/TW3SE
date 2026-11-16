-- Every object of a class: w3.all walks the engine's object table.

test("w3.all finds the player among the players", function()
  local p = w3.player(); if not p then return end
  local all = w3.all("CPlayer")
  check(#all >= 1, "at least one player")
  local found = false
  for _, o in ipairs(all) do if o == p then found = true end end
  check(found, "the player is in w3.all('CPlayer')")
end)

test("w3.all leaves the patterns inside entity templates out", function()
  local p = w3.player(); if not p then return end
  local mine = w3.all(w3.classof(p))
  eq(#mine, 1)
  check(mine[1] == p, "the one is the player")
  check(#w3.all(w3.classof(p), 1000, true) > 1, "with templates: the template patterns too")
end)

test("w3.all finds objects that are no entities", function()
  local p = w3.player(); if not p then return end
  local lm = w3.all("W3LevelManager")
  check(#lm >= 1, "a level manager")
  eq(w3.classof(lm[1]), "W3LevelManager")
  check(#w3.all("CComponent", 50) == 50, "components, limited to 50")
end)

test("w3.all takes subclasses along and refuses a wrong name", function()
  local p = w3.player(); if not p then return end
  local actors = w3.all("CActor")
  local players = w3.all("CPlayer")
  check(#actors > #players, "more actors than players")
  is_error(function() w3.all("NoSuchClass") end, "no class")
  eq(#w3.all("CActor", 0), 0)
end)

test("a spawned object is in w3.all", function()
  local p = w3.player(); if not p then return end
  local o = w3.new("TW3SE_LatentHost")
  if not o then return end
  local found = false
  for _, x in ipairs(w3.all("TW3SE_LatentHost")) do if x == o then found = true end end
  check(found, "the object made by w3.new is there")
end)
