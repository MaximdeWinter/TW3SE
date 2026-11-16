-- The TW3SE Menu's mod list: w3.mod{...} registers a mod with its settings; the values are kept in tw3se\vars.txt
-- and come back at the next registration; cfg.key = v sets one and runs on_change. The window itself is for the eye.

local NAME = "luatest mod settings"

local function clean()
  for _, k in ipairs({"on", "level", "mode", "greeting", "howl", "count"}) do
    w3._var_set("mod/" .. NAME .. "/" .. k, nil)
    w3._var_set("mod/" .. NAME .. "/" .. k .. ".default", nil)
  end
  w3._modreg[NAME] = nil
end

local function spec(changes)
  return {
    name = NAME, version = "1.0", author = "luatest", description = "a test",
    game_menu = false,  -- no page in the game's Options > Mods (it showed at the next start)
    settings = {
      {key = "on", default = true},
      {key = "level", type = "slider", min = 1, max = 50, int = true, default = 10},
      {key = "mode", type = "choice", items = {"Calm", "Wild"}},
      {type = "header", label = "More"},
      {key = "greeting", default = "Hi"},
      {key = "howl", type = "key", default = "F7"},
      {key = "count", default = 3},
    },
    on_change = function(key, value, old) changes[#changes + 1] = key .. "=" .. tostring(value) .. "<" .. tostring(old) end,
  }
end

test("w3.mod gives the defaults and infers the types", function()
  clean()
  local cfg = w3.mod(spec({}))
  eq(cfg.on, true)
  eq(cfg.level, 10)
  eq(cfg.mode, "Calm")
  eq(cfg.greeting, "Hi")
  eq(cfg.howl, "F7")
  eq(cfg.count, 3)
  local reg = w3._modreg[NAME]
  eq(reg.settings[1].type, "bool")
  eq(reg.settings[4].type, "header")
  eq(reg.settings[5].type, "text")
  eq(reg.settings[7].type, "number")
  clean()
end)

test("a setting changed is kept, runs on_change and comes back at the next registration", function()
  clean()
  local changes = {}
  local cfg = w3.mod(spec(changes))
  cfg.level = 70        -- above max: clamped
  cfg.mode = "Wild"
  cfg.on = false
  cfg.greeting = "Hello there"
  eq(cfg.level, 50)
  eq(table.concat(changes, " "), "level=50<10 mode=Wild<Calm on=false<true greeting=Hello there<Hi")
  eq(w3._var_get("mod/" .. NAME .. "/level"), "50")
  eq(w3._var_get("mod/" .. NAME .. "/on"), "false")
  cfg.level = 50        -- the same value: no on_change
  eq(#changes, 4)
  local again = w3.mod(spec({}))
  eq(again.level, 50)
  eq(again.mode, "Wild")
  eq(again.on, false)
  eq(again.greeting, "Hello there")
  again.reset()
  eq(again.level, 10)
  eq(again.mode, "Calm")
  eq(w3._var_get("mod/" .. NAME .. "/level"), "10")
  clean()
end)

test("w3.mod refuses what does not fit", function()
  clean()
  local cfg = w3.mod(spec({}))
  local ok, err = pcall(function() cfg.mode = "Sleepy" end)
  check(not ok and err:find("does not fit"), err)
  ok, err = pcall(function() cfg.nothing = 1 end)
  check(not ok and err:find("no setting"), err)
  ok, err = pcall(w3.mod, {})
  check(not ok and err:find("a name is needed"), err)
  ok, err = pcall(w3.mod, {name = "x luatest", settings = {{key = "c", type = "choice"}}})
  check(not ok and err:find("needs items"), err)
  ok, err = pcall(w3.mod, {name = "x luatest", settings = {{key = "s", type = "slider"}}})
  check(not ok and err:find("min and max"), err)
  ok, err = pcall(w3.mod, {name = "x luatest", settings = {{type = "button", label = "b"}}})
  check(not ok and err:find("run = function"), err)
  w3._var_set("mod/" .. NAME .. "/level", "not a number")  -- a stored value that does not fit: the default
  eq(w3.mod(spec({})).level, 10)
  clean()
end)

test("F2's Mods tab lists a w3.mod mod and finds it by a setting's name", function()
  clean()
  w3.mod({name = NAME, author = "luatest", game_menu = false,
          settings = {{key = "on", default = true, label = "Zebra crossing"}}})
  package.loaded["lib.modmanager"] = nil
  local mm = require("lib.modmanager")
  local found
  for _, e in ipairs(mm.entries("zebra")) do if e.name == NAME then found = e end end
  check(found, "found by the setting's label")
  eq(found.kind, "Lua")
  eq(found.author, "luatest")
  check(#mm.entries("no mod is called like this 7f3a") == 0, "a search with no match is empty")
  clean()
end)

test("w3.mods lists the registered mods, w3._mod_drop takes a mod's entry out", function()
  clean()
  w3.mod(spec({}))
  local found = false
  for _, m in ipairs(w3.mods()) do if m.name == NAME then found = m.author == "luatest" end end
  check(found, "listed")
  w3._mod_drop(w3._modreg[NAME].owner)
  eq(w3._modreg[NAME], nil)
  clean()
end)

test("the store has the defaults for WitcherScript; a new default replaces an untouched one, not the player's", function()
  clean()
  local cfg = w3.mod(spec({}))
  eq(w3._var_get("mod/" .. NAME .. "/level"), "10")       -- TW3SE_GetVar reads it before anything is changed
  eq(w3._var_get("mod/" .. NAME .. "/on"), "true")
  cfg.on = false                                           -- the player's own value
  local s2 = spec({})
  s2.settings[2].default = 20                              -- the mod's next version: another default
  s2.settings[1].default = true
  local again = w3.mod(s2)
  eq(again.level, 20)
  eq(w3._var_get("mod/" .. NAME .. "/level"), "20")
  eq(again.on, false)
  clean()
end)

test("the TW3SE Menu's key is a setting of TW3SE's entry and counts at once", function()
  local reg = w3._modreg["TW3SE"]
  check(reg ~= nil, "TW3SE's entry")
  local before = w3._menu_key()
  local cfg = setmetatable({}, {__index = function(_, k) return reg.values[k] end})
  eq(cfg.menu_key, before)
  w3._mod_set("TW3SE", "menu_key", "Ctrl+F9")
  eq(w3._menu_key(), "Ctrl+F9")
  check(not pcall(w3._menu_key, "NoSuchKey"), "a name that is no key is refused")
  w3._mod_set("TW3SE", "menu_key", before)
  eq(w3._menu_key(), before)
end)
