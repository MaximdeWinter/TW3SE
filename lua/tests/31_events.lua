-- Script events: an event the engine raises from C++ (OnSpawned when an entity enters the world) reaches
-- w3.hook like a call from WitcherScript, the super chain inside it too; orig() hands on the event's struct argument
-- (SEntitySpawnData) and the event's body runs. A function with an out parameter runs over the caller's frame; that
-- one needs frames to pass and is in build/event_camera.py (OnGameCameraTick).

local WOLF = [[characters\npc_entities\monsters\wolf_lvl1.w2ent]]

local function ahead(m)
  local pl = w3.player()
  local pp, h = pl:GetWorldPosition(), math.rad(pl:GetHeading())
  return {X = pp.X - math.sin(h) * m, Y = pp.Y + math.cos(h) * m, Z = pp.Z, W = 1}
end

test("OnSpawned from the engine reaches a hook, with the super chain inside it", function()
  if not w3.player() then return end
  local seq, hs = {}, {}
  for _, cls in ipairs({"CNewNPC", "CActor", "CGameplayEntity"}) do
    hs[#hs + 1] = w3.hook(cls, "OnSpawned", function(self, args, orig)
      if w3.classof(self) ~= "CNewNPC" then return end  -- other entities spawn meanwhile
      seq[#seq + 1] = cls .. ">"
      local r = orig()
      seq[#seq + 1] = "<" .. cls
      return r
    end)
  end
  local ok, err = pcall(function()
    local e = w3.spawn(WOLF, ahead(6))
    check(e, "a wolf")
    eq(table.concat(seq, " "), "CNewNPC> CActor> CGameplayEntity> <CGameplayEntity <CActor <CNewNPC")
    check(e.effectManager ~= nil, "the event's body ran (the actor made its effect manager)")
    w3.destroy(e)
  end)
  for _, h in ipairs(hs) do w3.unhook(h) end
  check(ok, err)
end)

test("an event's struct argument reads in the hook and goes on through orig()", function()
  if not w3.player() then return end
  local seen
  local h = w3.hook("CNewNPC", "OnSpawned", function(self, args, orig)
    seen = seen or w3.classof(args[1]) .. " restored=" .. tostring(args[1].restored)
    return orig()
  end)
  local ok, err = pcall(function()
    local e = w3.spawn(WOLF, ahead(6))
    eq(seen, "SEntitySpawnData restored=false")
    check(e.abilityManager ~= nil, "the body ran with the struct")
    w3.destroy(e)
  end)
  w3.unhook(h)
  check(ok, err)
end)
