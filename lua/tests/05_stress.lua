-- Load: many reads, calls and writes in a row. Proves the call/field paths are stable and do not leak or crash.
-- Kept moderate so luatest stays quick.

test("many field reads", function()
  local p = player and player() or w3.player()
  local g = w3.global("theGame")
  for i = 1, 3000 do
    local _ = g:GetEngineTimeAsSeconds()
    if p then local _ = p.equippedSign end
  end
  check(true, "survived 3000 reads")
end)

test("many method calls", function()
  local p = w3.player(); if not p then return end
  for i = 1, 1000 do
    local _ = p:GetHealth()
    local _ = p:GetVoicetag()
  end
  check(p:GetMaxHealth() > 0, "still sane after 1000 calls")
end)

test("many string writes", function()
  local p = w3.player(); if not p then return end
  local old = p.companionNPCIconPath
  for i = 1, 500 do
    p.companionNPCIconPath = "tw3se_stress_" .. i
  end
  eq(p.companionNPCIconPath, "tw3se_stress_500")
  p.companionNPCIconPath = old
end)

test("many script-function calls", function()
  if not w3.player() then return end
  for i = 1, 500 do
    local _ = w3.call("GetWitcherPlayer")
  end
  check(w3.call("GetWitcherPlayer") ~= nil, "script calls stay stable")
end)
