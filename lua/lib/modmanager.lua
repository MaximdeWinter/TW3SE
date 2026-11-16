-- The Mods tab of the TW3SE Menu: every mod in one list, a search field over names and settings, the chosen
-- mod's settings on the right. The list holds the Lua mods (w3.mod, and the folders in tw3se\mods), the mods with a
-- page in the game's options menu (their settings are read and set through the game's CInGameConfigWrapper and
-- saved to the game's settings), the plugins, and the folders in the game's Mods. ui.cpp calls draw() inside the
-- Mods tab and tabs() inside the window's tab bar (a mod's own tab: w3.mod{tab = fn}).
local M = {}
local ui = w3.ui
local maker = require("lib.modmaker")  -- New mod, the editor window
local conflicts = require("lib.conflicts")  -- the files more than one mod brings

local ROOT = w3.dir() .. "\\..\\..\\.."
local CONFIG = ROOT .. "\\bin\\config\\r4game\\user_config_matrix\\pc"
local VANILLA = {audio = true, display = true, gameplay = true, gamma = true, graphics = true, hdr = true,
                 hidden = true, hud = true, input = true, localization = true, rendering = true}
local RED, HEAD = 0xFF7870FF, 0xC8D2FFFF
local PIN, UNPIN = "\u{E718}", "\u{E77A}"  -- from Windows' icon font, merged into the menu's font (ui.cpp)

local state = {search = "", selected = nil, capture = nil, error = nil, saving = false}
local cache = {}  -- the options menu pages (read once), the localised texts

-- The time each Lua mod spends per frame over the last seconds (w3.profile's totals, two looks 2 s apart; the
-- Profiler window's reset starts the look again). A mod above SLOW_MS shows it in the list.
local SLOW_MS = 1.0
local slow = {at = -10, prev = nil, prevTicks = 0, ms = {}}
local function measure()
  if w3.time() - slow.at < 2 then return end
  slow.at = w3.time()
  local ok, list, ticks = pcall(w3.profile)
  if not ok then return end
  local totals = {}
  for _, p in ipairs(list) do totals[p.mod] = (totals[p.mod] or 0) + p.ms end
  if slow.prev and ticks > slow.prevTicks then
    local dt = ticks - slow.prevTicks
    slow.ms = {}
    for mod, ms in pairs(totals) do
      local d = ms - (slow.prev[mod] or 0)
      if d > 0 then slow.ms[mod] = d / dt end
    end
  end
  slow.prev, slow.prevTicks = totals, ticks
end
local function ms_of(e) return e.key and slow.ms[e.key] end

-- Pinned mods: at the top of the list and a tab of their own in the TW3SE Menu; their ids are kept
-- in tw3se\vars.txt (ui/pinned, one per line).
local pins
local function pinned()
  if not pins then
    pins = {}
    for id in ((w3._var_get("ui/pinned") or "") .. "\n"):gmatch("([^\n]*)\n") do if id ~= "" then pins[id] = true end end
  end
  return pins
end
local function set_pin(id, on)
  pinned()[id] = on or nil
  local list = {}
  for k in pairs(pins) do list[#list + 1] = k end
  table.sort(list)
  w3._var_set("ui/pinned", table.concat(list, "\n"))
end


local function pretty(s)  -- "option_containerglow_allContainers" -> "Containerglow all Containers"
  s = tostring(s or ""):gsub("%.xml$", ""):gsub("^option_", ""):gsub("^mod(%u)", "%1")
  s = s:gsub("(%l)(%u)", "%1 %2"):gsub("(%u)(%u%l)", "%1 %2"):gsub("_", " "):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
  return (s:gsub("^%l", string.upper))
end

local function loc(key)  -- the game's text for a key, nil when it has none
  if key == nil or key == "" then return nil end
  cache.loc = cache.loc or {}
  local t = cache.loc[key]
  if t == nil then
    local ok, r = pcall(w3.call, "GetLocStringByKeyExt", key)
    t = (ok and type(r) == "string" and r ~= "") and r or false
    cache.loc[key] = t
  end
  return t or nil
end

local function wrapper()
  return w3.object(w3.addr(w3.global("theGame"):GetInGameConfigWrapper()), "CInGameConfigWrapper")
end

local function save_settings_later()  -- the game's user settings file, once after a burst of changes
  if state.saving then return end
  state.saving = true
  w3.after(0.5, function()
    state.saving = false
    pcall(function() w3.global("theGame"):SaveUserSettings() end)
  end)
end

-- --- the options menu: a mod's XML in bin\config\r4game\user_config_matrix\pc gives its pages and their defaults ---
local function parse_menu_xml(path)
  local f = io.open(path, "rb")
  if not f then return {} end
  local x = f:read("a")
  f:close()
  local groups = {}
  for attrs, body in x:gmatch("<Group(%s[^>]*)>(.-)</Group>") do
    local id = attrs:match('%sid="([^"]*)"')
    local disp = attrs:match('displayName="([^"]*)"') or ""
    if id and disp:find("^Mods%.") then
      local defaults = {}
      local p0 = body:match('<Preset%s[^>]-id="0"[^>]*>(.-)</Preset>')
      if p0 then
        for var, val in p0:gmatch('<Entry%s+varId="([^"]*)"%s+value="([^"]*)"') do defaults[var] = val end
      end
      groups[#groups + 1] = {id = id, display = disp, defaults = defaults}
    end
  end
  return groups
end

local function page_title(g)
  local last = g.display:match("([^%.]+)$") or g.id
  return loc("panel_" .. last) or pretty(last)
end

-- the vars of one page, read from the game once: name, label, type, options
local function page_vars(w, g)
  if g.vars then return g.vars end
  local vars = {}
  local gi = w:GetGroupIdx(g.id)
  if gi < 0 then
    g.vars = vars
    return vars
  end
  local names = {}
  for vi = 0, w:GetVarsNum(gi) - 1 do names[#names + 1] = tostring(w:GetVarName(gi, vi)) end
  local prefix = nil  -- what every var's display name starts with ("containerglow_"): left out of the labels
  for _, v in ipairs(names) do
    local dn = tostring(w:GetVarDisplayName(g.id, v)):gsub("^option_", "")
    local p = dn:match("^(%w+_)")
    if p then prefix = (prefix == nil or prefix == p) and p or false end
  end
  for _, v in ipairs(names) do
    local dn = tostring(w:GetVarDisplayName(g.id, v))
    local kind = tostring(w:GetVarDisplayType(g.id, v))
    local short = dn:gsub("^option_", "")
    if prefix then short = short:gsub("^" .. prefix:gsub("%p", "%%%0"), "") end
    local var = {name = v, kind = kind, label = loc("option_" .. dn) or loc(dn) or pretty(short),
                 visible = w:IsVarVisible(g.id, v)}
    local n = w:GetVarOptionsNum(g.id, v)
    if kind == "SLIDER" and n >= 3 then
      var.min = tonumber(tostring(w:GetVarOption(g.id, v, 0))) or 0
      var.max = tonumber(tostring(w:GetVarOption(g.id, v, 1))) or 1
      var.steps = tonumber(tostring(w:GetVarOption(g.id, v, 2))) or 100
    elseif kind == "OPTIONS" then
      var.options = {}
      for i = 0, n - 1 do
        local k = tostring(w:GetVarOption(g.id, v, i))
        var.options[#var.options + 1] = loc("option_" .. k) or loc(k) or pretty(k)
      end
    end
    vars[#vars + 1] = var
  end
  g.vars = vars
  return vars
end

local function menu_mods()
  if cache.menu then return cache.menu end
  local list = {}
  for _, file in ipairs(w3._list(CONFIG, false) or {}) do
    local base = file:match("^(.-)%.[xX][mM][lL]$")
    if base and not VANILLA[base:lower()] and not base:lower():find("^tw3se_") then  -- tw3se_*: a Lua mod's own page
      local pages = parse_menu_xml(CONFIG .. "\\" .. file)
      if #pages > 0 then list[#list + 1] = {base = base, file = file, pages = pages} end
    end
  end
  cache.menu = list
  return list
end

local function set_menu_var(g, v, text)
  local ok, err = pcall(function() wrapper():SetVarValue(g.id, v.name, text) end)
  if ok then save_settings_later() else state.error = tostring(err) end
end

local function fmt_number(n)
  if math.type(n) == "integer" or n == math.floor(n) then return tostring(math.floor(n)) end
  return string.format("%.4g", n)
end

local function draw_menu_page(w, g)
  local vars = page_vars(w, g)
  if #vars == 0 then
    ui.dim("This page is not in the game's options (the game did not load its XML).")
    return
  end
  for i, v in ipairs(vars) do
    if v.visible then
      ui.id(i, function()
        local cur = tostring(w:GetVarValue(g.id, v.name))
        if (cur == "" or cur == "-1") and g.defaults[v.name] then cur = g.defaults[v.name] end
        if v.kind == "TOGGLE" then
          local ch, nv = ui.checkbox(v.label, cur == "true")
          if ch then set_menu_var(g, v, nv and "true" or "false") end
        elseif v.kind == "SLIDER" then
          local n = tonumber(cur) or v.min
          local step = (v.max - v.min) / math.max(v.steps, 1)
          local ch, nv
          if step == math.floor(step) and v.min == math.floor(v.min) then
            ch, nv = ui.slider_int(v.label, math.floor(n + 0.5), math.floor(v.min), math.floor(v.max))
          else
            ch, nv = ui.slider(v.label, n, v.min, v.max)
            if ch and step > 0 then nv = v.min + math.floor((nv - v.min) / step + 0.5) * step end
          end
          if ch then set_menu_var(g, v, fmt_number(nv)) end
        elseif v.kind == "OPTIONS" and v.options and #v.options > 0 then
          local idx = (tonumber(cur) or 0) + 1
          if idx < 1 or idx > #v.options then idx = 1 end
          local ch, picked = ui.combo(v.label, v.options[idx], v.options)
          if ch then
            for k, o in ipairs(v.options) do
              if o == picked then set_menu_var(g, v, tostring(k - 1)) break end
            end
          end
        elseif v.kind == "SUBTLE_SEPARATOR" then
          ui.separator()
        else
          ui.text(v.label .. ": " .. cur)
          ui.tooltip("Set in the game's options menu (" .. v.kind .. ")")
        end
      end)
    end
  end
end

local function draw_menu_mod(m)
  local ok, w = pcall(wrapper)
  if not ok or not w then
    ui.text("The game's options are not available yet.", RED)
    return
  end
  if #m.pages == 1 then
    draw_menu_page(w, m.pages[1])
    return
  end
  for i, g in ipairs(m.pages) do
    ui.id(i, function()
      ui.collapsing(page_title(g), function() draw_menu_page(w, g) end)
    end)
  end
end

-- --- Lua mods with w3.mod{...} ---
local function set_lua(name, key, v)
  local ok, err = pcall(w3._mod_set, name, key, v)
  if not ok then state.error = tostring(err) end
end

local function draw_lua_settings(reg)
  local name = reg.info.name
  for i, s in ipairs(reg.settings) do
    ui.id(i, function()
      local label = w3.tr(s.label) or pretty(s.key or "")
      local v = s.key and reg.values[s.key]
      if s.type == "header" then
        ui.spacing()
        ui.text(label, HEAD)
        ui.separator()
      elseif s.type == "button" then
        if ui.button(label) then
          local ok, err = pcall(s.run)
          if not ok then state.error = tostring(err) end
        end
      elseif s.type == "bool" then
        local ch, nv = ui.checkbox(label, v)
        if ch then set_lua(name, s.key, nv) end
      elseif s.type == "slider" then
        local ch, nv
        if s.int then ch, nv = ui.slider_int(label, v, s.min, s.max)
        else ch, nv = ui.slider(label, v, s.min, s.max) end
        if ch then set_lua(name, s.key, nv) end
      elseif s.type == "number" then
        local ch, nv = ui.input_number(label, v)
        if ch then set_lua(name, s.key, nv) end
      elseif s.type == "choice" then
        local ch, nv = ui.combo(label, v, s.items)
        if ch then set_lua(name, s.key, nv) end
      elseif s.type == "text" then
        local ch, nv = ui.input_text(label, v, true)
        if ch then set_lua(name, s.key, nv) end
      elseif s.type == "key" then
        local id = name .. "/" .. s.key
        local waiting = state.capture == id
        if ui.button(waiting and "Press a key (Esc: cancel)" or (v ~= "" and v or "(none)")) then
          state.capture = waiting and nil or id
          waiting = state.capture == id
        end
        -- while it waits, the menu key is a key like any other (the menu stays open); said each frame, it ends by
        -- itself when the field is no longer drawn (a false from another key field would end this one's wait)
        if waiting then w3._key_capture(true) end
        ui.same_line()
        ui.text(label)
        if waiting then
          local k = ui.key()
          if k == "Escape" then state.capture = nil
          elseif k then set_lua(name, s.key, k) state.capture = nil end
          if not state.capture then w3._key_capture(false) end
        end
      end
      if s.tip and s.type ~= "header" then ui.tooltip(w3.tr(s.tip)) end
    end)
  end
  if #reg.settings > 0 then
    ui.spacing()
    if ui.button("Reset to defaults") then
      for _, s in ipairs(reg.settings) do
        if s.key then set_lua(name, s.key, s.default) end
      end
    end
  end
end

-- --- the list ---
-- A folder in Mods with a file somewhere in it (a mod manager leaves empty folders behind after an uninstall; the game
-- skips them, so the list does too)
local function has_files(dir, depth)
  local list = w3.fs.list(dir) or {}
  for _, e in ipairs(list) do
    if not e.dir then return true end
  end
  if depth <= 0 then return false end
  for _, e in ipairs(list) do
    if e.dir and has_files(dir .. "\\" .. e.name, depth - 1) then return true end
  end
  return false
end

local function words(...)
  local t = {}
  for i = 1, select("#", ...) do
    local s = select(i, ...)
    if s and s ~= "" then t[#t + 1] = tostring(s) end
  end
  return table.concat(t, " "):lower()
end

local function build()
  local now = os.time()
  if cache.entries and cache.built == now then return cache.entries end  -- once a second at most
  local entries, byOwner = {}, {}
  for name, reg in pairs(w3._modreg) do
    local labels = {}
    for _, s in ipairs(reg.settings) do labels[#labels + 1] = w3.tr(s.label) or s.key end
    local e = {id = "lua:" .. name, name = name, kind = reg.owner == "tw3se" and "TW3SE" or "Lua",
               version = reg.info.version, author = reg.info.author, description = w3.tr(reg.info.description), reg = reg,
               key = reg.owner}
    e.search = words(name, reg.info.author, w3.tr(reg.info.description), "lua", table.concat(labels, " "))
    entries[#entries + 1] = e
    if reg.owner ~= "" then byOwner[reg.owner] = e end
  end
  local luaFolders = {}  -- Mods\<modName> that hold a Lua mod (tw3se\init.lua): listed once, as the Lua mod
  for _, m in ipairs(w3._mods() or {}) do
    local inMods = (m.path or ""):match("\\Mods\\([^\\]+)\\tw3se$")
    if inMods then luaFolders[inMods:lower()] = true end
    local err = (m.error ~= "" and m.error) or (m.last_error and m.last_error ~= "" and m.last_error) or nil
    local e = byOwner[m.key]
    if e then
      e.path, e.error, e.loaded = m.path, err, m.loaded
    else
      local name = m.key:gsub("^mods/", ""):gsub("^autorun/", ""):gsub("%.lua$", "")
      entries[#entries + 1] = {id = "luafile:" .. m.key, name = name, kind = "Lua", path = m.path, error = err,
                               loaded = m.loaded, search = words(name, "lua", m.key), key = m.key}
    end
  end
  local byName = {}  -- a plugin named like a Lua mod is that mod's: one entry (its line names the plugin)
  for _, e in ipairs(entries) do
    if e.kind == "Lua" then byName[e.name:lower()] = e end
  end
  for _, p in ipairs(w3._plugins() or {}) do
    local e = byName[(p.name or ""):lower()]
    if e then
      e.plugin = p.file
      e.search = e.search .. " " .. words(p.file, "plugin")
      if p.state == "failed" and not e.error then e.error = "its plugin " .. p.file .. " did not load" end
    else
      entries[#entries + 1] = {id = "plugin:" .. p.file, name = p.name, kind = "Plugin", version = p.version,
                               author = p.author, path = p.file, loaded = p.state ~= "failed",
                               error = p.state == "failed" and "its load failed" or nil,
                               search = words(p.name, p.author, p.file, "plugin")}
    end
  end
  local folders = {}
  for _, f in ipairs(w3._list(ROOT .. "\\Mods", true) or {}) do folders[f:lower()] = f end
  for _, m in ipairs(menu_mods()) do
    local folder = folders[m.base:lower()] or folders[("mod" .. m.base):lower()]
    if folder then folders[folder:lower()] = nil end
    local e = {id = "menu:" .. m.file, name = pretty(m.base), kind = "WitcherScript", menu = m,
               path = folder and ("Mods\\" .. folder) or ("bin\\config\\r4game\\user_config_matrix\\pc\\" .. m.file),
               menuOnly = folder == nil and m.base:lower():find("^mod") ~= nil}
    local text = {}
    local okw, w = pcall(wrapper)
    for _, g in ipairs(m.pages) do
      text[#text + 1] = page_title(g)
      if okw and w then  -- the settings' names too (read once per page)
        local okv, vars = pcall(page_vars, w, g)
        for _, v in ipairs(okv and vars or {}) do text[#text + 1] = v.label end
      end
    end
    e.search = words(e.name, m.base, folder, "witcherscript options menu", table.concat(text, " "))
    entries[#entries + 1] = e
  end
  for _, f in pairs(folders) do
    if not luaFolders[f:lower()] and has_files(ROOT .. "\\Mods\\" .. f, 8) then
      entries[#entries + 1] = {id = "folder:" .. f, name = pretty(f), kind = "WitcherScript", path = "Mods\\" .. f,
                               search = words(f, pretty(f), "witcherscript")}
    end
  end
  table.sort(entries, function(a, b) return a.name:lower() < b.name:lower() end)
  cache.entries, cache.built = entries, now
  return entries
end

local function matches(e, q)
  for word in q:gmatch("%S+") do
    if not e.search:find(word, 1, true) then return false end
  end
  return true
end

local function draw_detail(e)
  ui.text(e.name, 0xFFFFFFFF)
  ui.same_line()
  local on = pinned()[e.id]
  if ui.button((on and UNPIN or PIN) .. "##pin") then set_pin(e.id, not on) end
  ui.tooltip(on and "Unpin: back into the list, the tab goes"
                or "Pin: at the top of the list, with a tab of its own")
  local line = {e.kind}
  if e.plugin then line[1] = e.kind .. " + plugin " .. e.plugin end
  if e.version and e.version ~= "" then line[#line + 1] = "version " .. tostring(e.version) end
  if e.author and e.author ~= "" then line[#line + 1] = "by " .. tostring(e.author) end
  ui.dim(table.concat(line, "  |  "))
  if e.path then ui.dim(e.path) end
  local ms = ms_of(e)
  if ms and ms >= 0.05 then
    ui.text(string.format("Uses %.2f ms of each frame (the last seconds)", ms), ms >= SLOW_MS and RED or nil)
  end
  if e.description and e.description ~= "" then
    ui.spacing()
    ui.wrap(e.description)
  end
  if e.error and e.error ~= "" then
    ui.spacing()
    ui.text(e.error, RED)
  end
  ui.separator()
  if e.reg then
    if #e.reg.settings == 0 then ui.dim("No settings.") end
    draw_lua_settings(e.reg)
    if type(e.reg.info.tab) == "function" then
      ui.spacing()
      ui.dim("This mod has a tab of its own: " .. (e.reg.info.tab_title or e.name))
    end
  elseif e.menu then
    if e.menuOnly then
      ui.wrap("Only its page in the game's options menu is installed (no folder " .. e.menu.base .. " in Mods). " ..
              "The settings are there, the mod may be gone.")
      ui.spacing()
    end
    draw_menu_mod(e.menu)
    ui.spacing()
    ui.dim("These settings are the ones of the game's options menu (Options > Mods).")
  elseif e.kind == "Lua" then
    ui.dim("This mod has no settings here (it does not call w3.mod).")
  else
    ui.dim("No settings here.")
  end
  if e.kind == "Lua" and e.path then  -- a Lua mod's files: its init.lua in the editor window, its folder
    ui.spacing()
    local file = e.path:lower():find("%.lua$") and e.path or (e.path .. "\\init.lua")
    if ui.button("Edit") then maker.edit(file) end
    ui.tooltip(file)
    ui.same_line()
    if ui.button("Open folder") then
      local p = w3.proc.run('explorer.exe "' .. file:match("^(.*)\\[^\\]+$") .. '"')
      if p then w3.proc.close(p) end  -- nothing to read from it
    end
  end
  if state.error then
    ui.spacing()
    ui.text(state.error, RED)
  end
end


function M.draw()
  local entries = build()
  measure()
  maker.draw_new()
  ui.same_line()
  conflicts.draw_button()
  ui.same_line()
  ui.width(-1)
  local _, s = ui.input_text("##search", state.search, false, "Search mods and settings")
  state.search = s
  local q = s:lower()
  local shown, chosen = {}, nil
  local top, rest = {}, {}
  for _, e in ipairs(entries) do
    if matches(e, q) then
      local into = pinned()[e.id] and top or rest
      into[#into + 1] = e
      if e.id == state.selected then chosen = e end
    end
  end
  for _, e in ipairs(top) do shown[#shown + 1] = e end
  for _, e in ipairs(rest) do shown[#shown + 1] = e end
  if not chosen then
    chosen = shown[1]
    state.selected = chosen and chosen.id
  end
  ui.child("modlist", 0, function()
    ui.dim(#shown == #entries and (#entries .. " mods") or (#shown .. " of " .. #entries .. " mods"))
    for i, e in ipairs(shown) do
      ui.id(i, function()
        local bad = e.error and e.error ~= ""
        local label = (pinned()[e.id] and PIN .. " " or "") .. (bad and "(!) " or "") .. e.name
        local ms = ms_of(e)
        if ms and ms >= SLOW_MS then label = label .. string.format("  (%.1f ms)", ms) end
        local clicked
        if bad then ui.style({text = RED}, function() clicked = ui.selectable(label, e.id == state.selected) end)
        else clicked = ui.selectable(label, e.id == state.selected) end
        if clicked then
          state.selected = e.id
          state.error, state.capture = nil, nil
        end
        ui.tooltip(e.kind .. (e.path and ("\n" .. e.path) or "") .. (bad and ("\n" .. e.error) or ""))
      end)
      if i == #top and #rest > 0 then ui.separator() end  -- after the pinned ones
    end
  end, 320)
  ui.same_line()
  ui.child("moddetail", 0, function()
    if chosen then draw_detail(chosen)
    elseif #entries == 0 then ui.dim("No mods found.")
    else ui.dim("No mod fits the search.") end
  end)
end

function M.tabs()
  local byId = {}
  for _, e in ipairs(build()) do byId[e.id] = e end
  local ids = {}
  for id in pairs(pinned()) do if byId[id] then ids[#ids + 1] = id end end
  table.sort(ids, function(a, b) return byId[a].name:lower() < byId[b].name:lower() end)
  for _, id in ipairs(ids) do
    ui.tab(byId[id].name .. "##pin:" .. id, function()
      ui.child("pin:" .. id, 0, function() draw_detail(byId[id]) end)
    end)
  end
  local names = {}
  for name, reg in pairs(w3._modreg) do
    if type(reg.info.tab) == "function" then names[#names + 1] = name end
  end
  table.sort(names, function(a, b) return a:lower() < b:lower() end)
  for _, name in ipairs(names) do
    local reg = w3._modreg[name]
    ui.tab((reg.info.tab_title or name) .. "##modtab:" .. name, function()
      local ok, err = pcall(reg.info.tab)
      if not ok then ui.text(tostring(err), RED) end
    end)
  end
end

function M.refresh()  -- the options menu read again (a new mod XML) and the texts
  cache = {}
end

function M.entries(search)  -- the list as the tab shows it (for tests): entries that fit the search
  local out = {}
  for _, e in ipairs(build()) do if matches(e, (search or ""):lower()) then out[#out + 1] = e end end
  return out
end

return M
