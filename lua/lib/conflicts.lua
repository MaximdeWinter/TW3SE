-- F2 > Mods > Conflicts: the files that more than one mod of the game's Mods folder brings, and which one counts.
-- Scripts are the files under content\scripts, game files the paths in the content\*.bundle tables
-- (w3._bundle_paths). The one that counts is the mod with the lowest Priority in mods.settings (Documents\The Witcher
-- 3), the first by name when they have the same or none. "Let it win" gives a mod a Priority below the others of its
-- conflict, in mods.settings (a copy of the file goes to mods.settings.tw3se_backup first), from the next start.
local M = {}
local ui = w3.ui
local ROOT = w3.dir() .. "\\..\\..\\.."
local RED, GREY, GREEN = 0xFF7870FF, 0xA0A0A0FF, 0x8CE08CFF
local state = {open = false, result = nil, msg = nil, filter = ""}

local function settings_path() return w3.fs.folder("documents") .. "\\The Witcher 3\\mods.settings" end

-- mods.settings: {order = {section...}, sec = {name = {lines..., Enabled, Priority}}}
local function read_settings()
  local text = w3.fs.read(settings_path()) or ""
  local s = {order = {}, sec = {}, text = text}
  local cur
  for line in (text .. "\n"):gmatch("([^\r\n]*)\r?\n") do
    local name = line:match("^%[(.-)%]%s*$")
    if name then
      cur = {lines = {}}
      s.sec[name:lower()] = cur
      s.order[#s.order + 1] = name
      cur.name = name
    elseif cur and line ~= "" then
      cur.lines[#cur.lines + 1] = line
      local k, v = line:match("^%s*(%w+)%s*=%s*(.-)%s*$")
      if k == "Enabled" then cur.enabled = v ~= "0" end
      if k == "Priority" then cur.priority = tonumber(v) end
    end
  end
  return s
end

local function write_settings(s)
  local out = {}
  for _, name in ipairs(s.order) do
    local sec = s.sec[name:lower()]
    out[#out + 1] = "[" .. name .. "]"
    for _, l in ipairs(sec.lines) do out[#out + 1] = l end
  end
  local path = settings_path()
  if not w3.fs.exists(path .. ".tw3se_backup") and s.text ~= "" then w3.fs.write(path .. ".tw3se_backup", s.text) end
  return w3.fs.write(path, table.concat(out, "\r\n") .. "\r\n")
end

local function set_priority(s, mod, p)
  local sec = s.sec[mod:lower()]
  if not sec then
    sec = {name = mod, lines = {"Enabled=1"}, enabled = true}
    s.sec[mod:lower()] = sec
    s.order[#s.order + 1] = mod
  end
  local done = false
  for i, l in ipairs(sec.lines) do
    if l:match("^%s*Priority%s*=") then sec.lines[i], done = "Priority=" .. p, true end
  end
  if not done then table.insert(sec.lines, 2, "Priority=" .. p) end
  sec.priority = p
end

local function walk(dir, rel, out)
  for _, e in ipairs(w3.fs.list(dir) or {}) do
    if e.dir then walk(dir .. "\\" .. e.name, rel .. e.name .. "\\", out)
    else out[#out + 1] = rel .. e.name end
  end
end

-- {mods = {name...}, files = {path -> {mod...}}, kinds = {path -> "script"|"file"}}, the mods in their order
function M.scan()
  local s = read_settings()
  local mods = {}
  for _, e in ipairs(w3.fs.list(ROOT .. "\\Mods") or {}) do
    local sec = s.sec[e.name:lower()]
    if e.dir and not (sec and sec.enabled == false) then
      mods[#mods + 1] = {name = e.name, priority = sec and sec.priority}
    end
  end
  table.sort(mods, function(a, b)
    local pa, pb = a.priority or math.huge, b.priority or math.huge
    if pa ~= pb then return pa < pb end
    return a.name:lower() < b.name:lower()
  end)
  local files, kinds = {}, {}
  local function add(path, mod, kind)
    local k = path:lower()
    files[k] = files[k] or {}
    local list = files[k]
    if list[#list] ~= mod then list[#list + 1] = mod end
    kinds[k] = kinds[k] or kind
  end
  for _, m in ipairs(mods) do
    local content = ROOT .. "\\Mods\\" .. m.name .. "\\content"
    local scripts = {}
    walk(content .. "\\scripts", "scripts\\", scripts)
    for _, p in ipairs(scripts) do if p:lower():find("%.ws$") then add(p, m.name, "script") end end
    for _, e in ipairs(w3.fs.list(content) or {}) do
      if not e.dir and e.name:lower():find("%.bundle$") then
        for _, p in ipairs(w3._bundle_paths(content .. "\\" .. e.name) or {}) do add(p, m.name, "file") end
      end
    end
  end
  local conflicts = {}  -- "modA|modB" -> {mods = {...}, scripts = {...}, files = {...}}
  for path, list in pairs(files) do
    if #list > 1 then
      local key = table.concat(list, "|")
      local c = conflicts[key] or {mods = list, scripts = {}, files = {}}
      conflicts[key] = c
      local into = kinds[path] == "script" and c.scripts or c.files
      into[#into + 1] = path
    end
  end
  local out = {}
  for _, c in pairs(conflicts) do
    table.sort(c.scripts)
    table.sort(c.files)
    out[#out + 1] = c
  end
  table.sort(out, function(a, b) return (#a.scripts * 1000 + #a.files) > (#b.scripts * 1000 + #b.files) end)
  return {mods = mods, conflicts = out}
end

function M.let_win(c, mod)  -- the mod gets a Priority below the others of its conflict (mods.settings)
  local s = read_settings()
  local low = math.huge
  for _, m in ipairs(c.mods) do
    local sec = s.sec[m:lower()]
    if m ~= mod and sec and sec.priority then low = math.min(low, sec.priority) end
  end
  if low == math.huge then low = 1 end
  if low <= 0 then  -- room below: every other mod of the conflict one up
    for _, m in ipairs(c.mods) do
      if m ~= mod then set_priority(s, m, ((s.sec[m:lower()] or {}).priority or 0) + 1) end
    end
    set_priority(s, mod, 0)
  else
    set_priority(s, mod, low - 1)
  end
  return write_settings(s)
end

local function draw_conflict(i, c)
  local title = table.concat(c.mods, ", ") .. "  (" .. (#c.scripts > 0 and (#c.scripts .. " script(s)") or "") ..
                ((#c.scripts > 0 and #c.files > 0) and ", " or "") .. (#c.files > 0 and (#c.files .. " file(s)") or "") .. ")"
  ui.id(i, function()
    ui.collapsing(title, function()
      ui.text("Counts: " .. c.mods[1], GREEN)
      if c.mods[1]:lower():find("mergedfiles") then ui.dim("(the scripts Script Merger merged from the others)") end
      for _, m in ipairs(c.mods) do
        if m ~= c.mods[1] then
          ui.id(m, function()
            if ui.button("Let " .. m .. " win") then
              local ok, err = M.let_win(c, m)
              state.msg = ok and (m .. " counts from the next start (mods.settings)") or tostring(err)
            end
            ui.tooltip("Gives it a lower Priority in mods.settings. Vortex may set its own order again at its next deploy.")
          end)
        end
      end
      for _, p in ipairs(c.scripts) do ui.text(p) end
      for j, p in ipairs(c.files) do
        if j > 200 then ui.dim("... " .. (#c.files - 200) .. " more") break end
        ui.dim(p)
      end
    end)
  end)
end

function M.draw_button()
  if ui.button("Conflicts") then
    state.open = not state.open
    if state.open then state.result, state.msg = nil, nil end
  end
  ui.tooltip("The files more than one mod brings, and which one counts")
  if not state.open then return end
  if not state.result then
    local ok, r = pcall(M.scan)
    state.result = ok and r or {mods = {}, conflicts = {}, err = tostring(r)}
  end
  local _, keep = ui.window("Mod conflicts", function()
    local r = state.result
    if ui.button("Look again") then state.result = nil return end
    ui.same_line()
    ui.dim(#r.mods .. " mods, " .. #r.conflicts .. " conflict(s)")
    if r.err then ui.text(r.err, RED) end
    if state.msg then ui.text(state.msg, GREY) end
    ui.separator()
    if #r.conflicts == 0 then ui.text("No file is in more than one mod.", GREEN) end
    ui.child("conflicts", 0, function()
      for i, c in ipairs(r.conflicts) do draw_conflict(i, c) end
    end)
  end, 760, 520, true)
  if keep == false then state.open = false end  -- its X
end

return M
