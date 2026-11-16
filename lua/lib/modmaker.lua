-- New mods from the TW3SE Menu (F2 > Mods > New mod) and a window to edit a mod's files in the game. A new mod is a
-- folder Mods\mod<Name>\tw3se\ with an init.lua from the template or one of tw3se\lua\examples; it loads within a
-- second (the mod watch), and every save in the editor loads it again. modmanager.lua calls draw_new() and edit().
local M = {}
local ui = w3.ui
-- the game folder without "..": the mod watch names a mod by its real path (w3._mods), the editor compares with it
local ROOT = (w3.dir():gsub("\\[^\\]+\\[^\\]+\\[^\\]+$", ""))
local EXAMPLES = w3.dir() .. "\\lua\\examples"
local RED, GREY, GREEN = 0xFF7870FF, 0xA0A0A0FF, 0x8CE08CFF

local TEMPLATE = [[
-- %s: what it does in one line.
-- This file runs when the game starts. Save it and the mod loads again within a second.

local saved = w3.saved()   -- this mod's data in the current save
local cfg                  -- its settings (w3.mod below)
local h                    -- the key's handle

local function run()
  if not cfg.enabled then return end
  saved.count = (saved.count or 0) + 1
  w3.hud.message("Hello from %s (" .. saved.count .. " times in this save)", 2)
end

cfg = w3.mod{
  name = "%s", version = "1.0", author = "you",
  tw3se = "%s",
  description = "What it does.",
  settings = {
    {key = "enabled", default = true, label = "Enabled"},
    {key = "key", type = "key", default = "F7", label = "Key"},
  },
  on_change = function(key, value)
    if key == "key" then
      w3.off_key(h)
      h = w3.on_key(value, run)
    end
  end,
}

h = w3.on_key(cfg.key, run)
]]

local new = {open = false, name = "", from = 1, err = nil}
local editors = {}  -- path -> {text, saved, msg}

local function folder_name(name)  -- "Better Wolves" -> "modBetterWolves"
  local s = name:gsub("[^%w]", "")
  if s == "" then return nil end
  if not s:lower():find("^mod") then s = "mod" .. s end
  return s
end

local function sources()  -- what a new mod starts from: the template, then the examples
  local list = {{label = "Template (settings and a key)"}}
  for _, e in ipairs(w3.fs.list(EXAMPLES) or {}) do
    if not e.dir and e.name:match("%.lua$") then list[#list + 1] = {label = "Example: " .. e.name, file = e.name} end
  end
  return list
end

local uiHandle
function M.edit(path)  -- opens a file in the editor window (drawn while the TW3SE Menu is open, on any tab)
  if not uiHandle then uiHandle = w3._as("tw3se", w3.on_ui, function() M.draw_editors() end) end
  if not editors[path] then
    local text = w3.fs.read(path) or ""
    editors[path] = {text = text, saved = text}
  end
  editors[path].open = true
end

local function create()
  local name = new.name:match("^%s*(.-)%s*$")
  local folder = folder_name(name)
  if not folder then new.err = "A name with letters or digits" return end
  local dir = ROOT .. "\\Mods\\" .. folder .. "\\tw3se"
  if w3.fs.exists(dir .. "\\init.lua") then new.err = "Mods\\" .. folder .. " has an init.lua already" return end
  local src = sources()[new.from]
  local text
  if src.file then
    text = w3.fs.read(EXAMPLES .. "\\" .. src.file) or ""
  else
    local q = (name:gsub('"', "'"):gsub("\\", "/"))  -- it goes into Lua strings of the new file: no " and no \
    text = string.format(TEMPLATE, name, q, q, (w3.version()))
  end
  local ok, err = w3.fs.mkdir(dir)
  if ok then ok, err = w3.fs.write(dir .. "\\init.lua", text) end
  if not ok then new.err = tostring(err) return end
  new.open, new.err, new.name = false, nil, ""
  M.edit(dir .. "\\init.lua")
  return "mods/" .. folder
end

function M.draw_new()  -- the button and its window; returns the new mod's key once it is made
  if ui.button("New mod") then new.open = not new.open end
  ui.tooltip("A mod of your own in Mods, from the template or an example")
  if not new.open then return nil end
  local made
  local _, keep = ui.window("New mod", function()
    ui.width(320)
    local _, n = ui.input_text("Name", new.name, false, "Better Wolves")
    new.name = n
    local list, labels = sources(), {}
    for i, s in ipairs(list) do labels[i] = s.label end
    ui.width(320)
    local ch, picked = ui.combo("Start from", labels[new.from] or labels[1], labels)
    if ch then for i, l in ipairs(labels) do if l == picked then new.from = i end end end
    local folder = folder_name(new.name or "")
    ui.dim(folder and ("Mods\\" .. folder .. "\\tw3se\\init.lua") or "Mods\\mod<Name>\\tw3se\\init.lua")
    if ui.button("Create") then made = create() end
    ui.same_line()
    if ui.button("Cancel") then new.open, new.err = false, nil end
    if new.err then ui.text(new.err, RED) end
  end, 460, 190, true)
  if keep == false then new.open, new.err = false, nil end  -- its X
  return made
end

local function error_of(path)  -- the mod's error from the mod watch (load or run), for the editor's foot line
  local dir = path:match("^(.*)\\[^\\]+$")
  for _, m in ipairs(w3._mods() or {}) do
    if m.path == dir then
      local e = (m.error ~= "" and m.error) or (m.last_error ~= "" and m.last_error) or nil
      return e, m.loaded
    end
  end
end

function M.draw_editors()  -- every open editor window (modmanager draws them each frame)
  for path, ed in pairs(editors) do
    if ed.open then
      local _, keep = ui.window("Edit: " .. path:match("([^\\]+\\[^\\]+)$") .. "##" .. path, function()
        local dirty = ed.text ~= ed.saved
        local ctrl = ui.mods()
        local save = ui.button("Save") or (dirty and ctrl and ui.pressed("S"))
        ui.tooltip("Ctrl+S; the mod loads again within a second")
        ui.same_line()
        if ui.button("Revert") then ed.text = ed.saved ed.msg = nil end
        ui.same_line()
        if ui.button("Open folder") then
          local p = w3.proc.run('explorer.exe "' .. path:match("^(.*)\\[^\\]+$") .. '"')
          if p then w3.proc.close(p) end  -- nothing to read from it
        end
        ui.same_line()
        if ui.button("Close") then ed.open = false end
        ui.same_line()
        ui.dim(dirty and "changed" or "saved")
        local _, t = ui.input_multiline("##text", ed.text, -1, -46)
        ed.text = t
        if save then
          local ok, err = w3.fs.write(path, ed.text)
          if ok then ed.saved, ed.msg, ed.at = ed.text, nil, w3.time() else ed.msg = tostring(err) end
        end
        local err, loaded = error_of(path)
        if ed.msg then ui.text(ed.msg, RED)
        elseif err and (not ed.at or w3.time() - ed.at > 1.5) then ui.text(err, RED)
        elseif ed.at and w3.time() - ed.at < 1.5 then ui.text("Saved, the mod loads again ...", GREY)
        elseif loaded then ui.text("Loaded", GREEN) end
      end, 820, 620, true)
      if keep == false then ed.open = false end  -- its X
    end
  end
end

M._test = {folder_name = folder_name, create = create, new = new}  -- tests/offline/modmaker.lua
return M
