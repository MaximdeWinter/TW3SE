-- Reads and calls that need a loaded save. These tests skip themselves (pass) when no player is present, so the
-- same set runs in the menu and in-world; lua_run.py / luatest run them after a save is loaded.
local function player()
  return w3.player()
end

test("player class and fields", function()
  local p = player(); if not p then return end
  eq(w3.classof(p), "W3PlayerWitcher")
  check(type(p.equippedSign) == "string", "equippedSign is an enum name")
  check(#p.alchemyRecipes >= 0, "alchemyRecipes array")
end)

test("native methods", function()
  local p = player(); if not p then return end
  eq(p:GetVoicetag(), "GERALT")
  check(type(p:IsInCombat()) == "boolean", "IsInCombat")
  check(type(p:GetAppearance()) == "string", "GetAppearance")
end)

test("struct value and read", function()
  local p = player(); if not p then return end
  local pos = p:GetWorldPosition()
  check(type(pos.X) == "number" and type(pos.Y) == "number", "vector fields")
end)

test("field write round-trip", function()
  local p = player(); if not p then return end
  local v = p.equippedSign
  p.equippedSign = v
  eq(p.equippedSign, v)
end)

test("array view read and element write", function()
  local p = player(); if not p then return end
  local a = p.alchemyRecipes            -- array<CName>, a live view
  check(#a >= 0, "length")
  if #a > 0 then
    local first = a[1]
    check(type(first) == "string", "CName element is a string")
    a[1] = first                        -- round-trip: write the same value back
    eq(a[1], first)
    eq(#a:to_table(), #a)               -- snapshot to a plain table
  end
  is_error(function() a[#a + 5] = "x" end, "out of range")  -- a hole past the end is still refused
end)

test("array grow and shrink (names)", function()
  local p = player(); if not p then return end
  local a = p.alchemyRecipes            -- array<CName>, a live view of 4-byte elements
  local n0 = #a
  a:insert("tw3se_test_recipe")         -- append
  eq(#a, n0 + 1)
  eq(a[#a], "tw3se_test_recipe")
  eq(a:remove(), "tw3se_test_recipe")   -- remove the last, returns it
  eq(#a, n0)
  a[#a + 1] = "tw3se_test_2"            -- a[#a+1] = v appends too
  eq(a[#a], "tw3se_test_2")
  a:remove(#a)
  eq(#a, n0)
  if n0 >= 1 then                       -- insert at a position shifts the rest up
    local first = a[1]
    a:insert(1, "tw3se_front")
    eq(a[1], "tw3se_front")
    eq(a[2], first)
    eq(#a, n0 + 1)
    a:remove(1)
    eq(a[1], first)
    eq(#a, n0)
  end
end)

test("string field write", function()
  local p = player(); if not p then return end
  local old = p.companionNPCIconPath
  p.companionNPCIconPath = "tw3se_test_icon"
  eq(p.companionNPCIconPath, "tw3se_test_icon")
  p.companionNPCIconPath = ""
  eq(p.companionNPCIconPath, "")
  p.companionNPCIconPath = old  -- put it back
  eq(p.companionNPCIconPath, old)
end)

test("call with struct argument", function()
  if not player() then return end
  near(w3.call("VecLength", {X = 3, Y = 4, Z = 0, W = 0}), 5)
  near(w3.call("VecDistance", {X = 0, Y = 0, Z = 0, W = 1}, {X = 0, Y = 6, Z = 8, W = 1}), 10)
end)

test("call with object argument", function()
  local p = player(); if not p then return end
  check(tostring(p:GetAttitude(p)):find("AIA_"), "attitude is an enum name")
  is_error(function() p:GetAttitude(w3.global("theGame")) end, "is needed")
end)

test("call with enum argument", function()
  local p = player(); if not p then return end
  local k = next(w3.enum("EGameplayInfoCacheType"))
  check(type(p:GetGameplayInfoCache(k)) == "boolean", "enum arg call")
end)

test("handle field write round-trip", function()
  local p = player(); if not p then return end
  local lm = p.levelManager            -- handle:W3LevelManager, should be populated
  if lm then
    eq(w3.classof(lm), "W3LevelManager")
    p.levelManager = lm                 -- write the same handle back
    eq(w3.classof(p.levelManager), "W3LevelManager")
    check(p.levelManager == lm, "same object after write")
  end
  is_error(function() p.levelManager = w3.global("theGame") end, "is needed")  -- wrong class refused
end)

test("call a global script function", function()
  local p = player(); if not p then return end
  local w = w3.call("GetWitcherPlayer")   -- a pure WitcherScript function, run through the sink
  check(w, "GetWitcherPlayer returned nil")
  eq(w3.classof(w), "W3PlayerWitcher")
  check(w == p, "same player object")
end)

test("hook a native method: observe self and override the return", function()
  local p = player(); if not p then return end
  eq(p:GetVoicetag(), "GERALT")
  local seen = nil
  local h = w3.hook("CActor", "GetVoicetag", function(self, args, orig)
    seen = w3.classof(self)
    return "HOOKED"                                  -- replace the return
  end)
  eq(p:GetVoicetag(), "HOOKED")
  check(seen and seen:find("Player"), "the hook saw the player as self")
  w3.unhook(h)
  eq(p:GetVoicetag(), "GERALT")                     -- restored
end)

test("hook a WitcherScript method: observe the original and override it (D2)", function()
  local p = player(); if not p then return end
  local real = p:GetHealth()               -- GetHealth is a pure WitcherScript method (no native handler)
  check(type(real) == "number" and real > 0, "GetHealth returns the health")
  -- observe: run the original body through the dispatcher and pass its value through
  local seen
  local h = w3.hook("CActor", "GetHealth", function(self, args, orig) seen = orig(); return seen end)
  local passthrough = p:GetHealth()
  w3.unhook(h)
  near(seen, real)                          -- the hook saw the real health from orig()
  near(passthrough, real)                   -- and passed it through unchanged
  -- replace: force a value without calling orig
  local h2 = w3.hook("CActor", "GetHealth", function(self, args, orig) return 123.0 end)
  near(p:GetHealth(), 123.0)
  w3.unhook(h2)
  near(p:GetHealth(), real)                 -- restored after unhook
end)

test("hook a WitcherScript global function (D2)", function()
  local p = player(); if not p then return end
  local calls = 0
  local h = w3.hook("GetWitcherPlayer", function(self, args, orig) calls = calls + 1; return orig() end)
  local w = w3.call("GetWitcherPlayer")     -- a pure WitcherScript global, now routed through the hook
  w3.unhook(h)
  check(calls >= 1, "the script-function hook ran")
  check(w and tostring(w3.classof(w)):find("Player"), "orig() returned the real player")
  check(w3.call("GetWitcherPlayer") == w, "unhook restored the original")
end)

test("this-adjust: IScriptable state methods get the right this", function()
  local p = player(); if not p then return end
  -- GetCurrentStateName and IsInState are IScriptable state-machine methods; their handler expects this+16 (the
  -- method table's this-adjust). Applied correctly, the player's current state name round-trips through IsInState;
  -- a wrong this would give a garbage name (IsInState false) or fault.
  local s = p:GetCurrentStateName()
  check(type(s) == "string", "a state name comes back")
  check(type(p:IsInState(s)) == "boolean", "IsInState returns a bool")
  if s ~= "" then eq(p:IsInState(s), true) end     -- the player is in its own current state
  eq(w3.call("StrLen", "witcher"), 7)              -- the VM is healthy after the adjusted-this calls
end)

test("call a WitcherScript method on an object", function()
  local p = player(); if not p then return end
  -- pure script methods, run through the sink with the object as context; the returned components must be the
  -- player's own (a wrong/null context would give nil), which proves the context propagates
  eq(w3.classof(p:GetHorseManager()), "W3HorseManager")
  eq(w3.classof(p:GetAssociatedInventory()), "CInventoryComponent")
  eq(w3.classof(p:GetMedallion()), "W3MedallionController")
end)

test("out-array parameter: GetWeatherNames fills the list", function()
  if not (w3.global("theGame") and w3.global("theGame"):IsActive()) then return end
  -- GetWeatherNames(out names : array<name>) : bool - the filled array is returned as the second value
  local ok, names = w3.call("GetWeatherNames", {})
  check(type(names) == "table", "names is a table")
  check(#names > 0, "weather names are not empty")
  check(type(names[1]) == "string", "a weather name is a CName string")
  -- the VM must stay healthy after an out-param call (a wrong encoding used to corrupt it)
  eq(w3.call("StrLen", "witcher"), 7)
end)

test("out-array parameter: GetEntitiesByTag returns entities", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  local ents = g:GetEntitiesByTag("PLAYER", {})   -- the player is tagged PLAYER
  check(type(ents) == "table", "entities is a table")
  check(#ents >= 1 and w3.classof(ents[1]):find("Player"), "the player is found by its PLAYER tag")
end)

test("out-scalar by-reference fills string and name", function()
  local p = player(); if not p then return end
  -- GetTemplatePathAndAppearance(out string templatePath, out name appearance): both come back as extra values,
  -- in parameter order, after the (void) return. Pass placeholders for the two out slots.
  local path, appr = p:GetTemplatePathAndAppearance("", "")
  check(type(path) == "string", "templatePath is a string")
  check(type(appr) == "string", "appearance is a CName string")
  check(#path > 0, "the player has a template path")
  -- the VM must stay healthy after an out-scalar call
  eq(w3.call("StrLen", "witcher"), 7)
end)

test("entities enumeration", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  local all = w3.entities()
  check(type(all) == "table", "entities is a table")
  check(#all > 0, "some pre-placed entities are loaded")
  check(w3.classof(all[1]) ~= nil, "an entity has a class")
  -- filtered by class: every result is that class or a subclass
  local actors = w3.entities("CActor")
  check(#actors <= #all, "filter returns a subset")
  -- near: entities within a large radius of the player is a subset of all, and within a tiny radius is fewer
  local wide = w3.near(100000)
  check(#wide > 0 and #wide <= #all, "near(large) is a non-empty subset")
  check(#w3.near(1) <= #wide, "near(1) is no larger")
end)

test("find by tag: the player is tagged PLAYER", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  local list = w3.by_tag("PLAYER")
  check(type(list) == "table", "by_tag returns a table")
  local found = false
  for _, e in ipairs(list) do if w3.classof(e):find("Player") then found = true end end
  check(found, "the player is found by its PLAYER tag")
end)

test("entity guid round-trip (w3.guid / w3.by_guid)", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  -- find a pre-placed entity that carries a lasting guid (run-time ones return nil)
  local guid, ent
  for _, e in ipairs(w3.entities()) do
    local gu = w3.guid(e)
    if gu then guid, ent = gu, e; break end
  end
  if not guid then return end  -- no pre-placed entity loaded here, skip
  check(guid:find("%x+%-%x+%-%x+%-%x+"), "guid looks like four hex dwords")
  local back = w3.by_guid(guid)
  check(back, "by_guid finds the entity")
  check(back == ent, "same object by guid")
  eq(w3.guid(back), guid)
  check(w3.by_guid("00000000-00000000-00000000-00000000") == nil, "a zero guid finds nothing")
  is_error(function() w3.by_guid("not-a-guid") end, "guid")
end)

test("near a position returns a subset", function()
  local p = w3.player(); if not p then return end
  local pos = p:GetWorldPosition()
  local around_pos = w3.near(pos, 100000)
  local around_player, errs = {}, {}  -- w3.near(100000) written out, with its errors counted
  local center = p:GetWorldPosition()
  for _, e in ipairs(w3.entities()) do
    local ok, d = pcall(function() return w3.call("VecDistance", center, e:GetWorldPosition()) end)
    if ok then around_player[#around_player + 1] = e
    else local m = tostring(d):gsub("^.-VecDistance", "VecDistance"):sub(1, 200) errs[m] = (errs[m] or 0) + 1 end
  end
  check(#around_pos > 0, "near a position finds entities")
  -- the player's position is the player; each call scans the loaded entities again, and right after a load the
  -- world still streams sectors in and out (3639 and 3488 in Kaer Morhen), so the counts may differ a little
  local why = ""
  for m, c in pairs(errs) do why = why .. " / " .. c .. "x " .. m end
  check(math.abs(#around_pos - #around_player) <= #around_pos * 0.1,
        "around the player = around its position (" .. #around_pos .. " vs " .. #around_player .. ", all " ..
        #w3.entities() .. ")" .. why)
  check(#w3.near(pos, 1, "CActor") <= #around_pos, "class + tiny radius is a subset")
  check(#w3.near(100000) > 0, "near(radius) around the player finds entities")
end)

test("world object", function()
  local g = w3.global("theGame")
  if not (g and g:IsActive()) then return end
  check(g.activeWorld, "activeWorld")
  check(tostring(g.activeWorld:GetDepotPath()):find("%.w2w"), "world path")
end)
