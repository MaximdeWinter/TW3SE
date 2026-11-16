-- The profiler window of the in-game UI (the console's "Profiler" box), two tabs: Lua, the time each Lua mod spends
-- by kind (on_tick, hooks, natives ...) over the last second (w3.profile); WitcherScript, the time of each script
-- function, its own and its whole, with the mod its file belongs to (w3.script_profile, on while the tab counts).
local M = {}
local handle
local prev, prevTicks = {}, 0  -- the totals of the last refresh
local rows, ticks = {}, 0      -- what the window shows
local nextAt = 0
local script = {rows = {}, seconds = 0, on = false, nextAt = 0}
local GREY = 0xA0A0A0FF
local RED = 0xFF8070FF

local function refresh()
  local list, t = w3.profile()
  local now = {}
  rows = {}
  local dt = t - prevTicks
  for _, e in ipairs(list) do
    local key = e.mod .. "|" .. e.kind
    local p = prev[key]
    local ms = e.ms - (p and p.ms or 0)
    local calls = e.calls - (p and p.calls or 0)
    now[key] = { ms = e.ms, calls = e.calls }
    if calls > 0 then
      rows[#rows + 1] = { mod = e.mod ~= "" and e.mod or "(none)", kind = e.kind, per_tick = dt > 0 and ms / dt or 0,
                          calls = calls, max = e.max }
    end
  end
  table.sort(rows, function(a, b) return a.per_tick > b.per_tick end)
  prev, prevTicks, ticks = now, t, dt
end

local function draw_lua()
  local total = 0
  for _, r in ipairs(rows) do total = total + r.per_tick end
  w3.ui.text(string.format("Lua over the last second: %.3f ms per tick, %d tick(s)", total, ticks), total > 2 and RED or GREY)
  w3.ui.child("rows", 0, function()
    w3.ui.table("p", { "Mod", "Kind", "ms per tick", "Calls", "Slowest call" }, function()
      for i, r in ipairs(rows) do
        if i > 60 then break end
        w3.ui.row()
        w3.ui.col() w3.ui.text(r.mod)
        w3.ui.col() w3.ui.text(r.kind, GREY)
        w3.ui.col() w3.ui.text(string.format("%.3f", r.per_tick), r.per_tick > 1 and RED or nil)
        w3.ui.col() w3.ui.text(tostring(r.calls))
        w3.ui.col() w3.ui.text(string.format("%.3f", r.max))
      end
    end)
  end)
end

local function draw_script()
  if w3.time() >= script.nextAt then
    local ok, list, seconds, on = pcall(w3.script_profile)
    if ok then script.rows, script.seconds, script.on = list, seconds, on end
    script.nextAt = w3.time() + 1
  end
  if w3.ui.button(script.on and "Stop" or "Count") then
    pcall(w3.script_profile, script.on and "off" or "on")
    script.nextAt = 0
  end
  w3.ui.tooltip("Counts the time of every WitcherScript function while it is on (it costs a little itself)")
  w3.ui.same_line()
  if w3.ui.button("Reset") then pcall(w3.script_profile, "reset") script.nextAt = 0 end
  w3.ui.same_line()
  w3.ui.dim(string.format("%s, %.0f s counted; ms per second of play", script.on and "counting" or "stopped", script.seconds))
  local s = math.max(script.seconds, 0.001)
  w3.ui.child("srows", 0, function()
    w3.ui.table("s", { "Own", "Whole", "Calls/s", "Function", "Mod" }, function()
      for i, r in ipairs(script.rows) do
        if i > 80 then break end
        local own = r.own / s
        w3.ui.row()
        w3.ui.col() w3.ui.text(string.format("%.2f", own), own > 2 and RED or nil)
        w3.ui.col() w3.ui.text(string.format("%.2f", r.whole / s), GREY)
        w3.ui.col() w3.ui.text(string.format("%.0f", r.calls / s), GREY)
        w3.ui.col() w3.ui.text(r.fn)
        w3.ui.tooltip(r.file ~= "" and r.file or "(native)")
        w3.ui.col() w3.ui.text(r.mod ~= "" and r.mod or (r.file == "" and "(native)" or ""), GREY)
      end
    end)
  end)
end

local function draw()
  if w3.time() >= nextAt then
    refresh()
    nextAt = w3.time() + 1
  end
  local _, keep = w3.ui.window("TW3SE Profiler", function()
    w3.ui.tabs("profiler", function()
      w3.ui.tab("Lua", draw_lua)
      w3.ui.tab("WitcherScript", draw_script)
    end)
  end, 760, 420, true)
  if keep == false then  -- its X: the console's Profiler box goes off with it
    if script.on then pcall(w3.script_profile, "off") script.on = false end  -- no counting nobody sees (it costs)
    M.show(false)
    if w3._ui_flag then w3._ui_flag("profiler", false) end
  end
end

function M.show(on)
  if on and not handle then
    local list, t = w3.profile()
    prev, prevTicks = {}, t
    for _, e in ipairs(list) do prev[e.mod .. "|" .. e.kind] = { ms = e.ms, calls = e.calls } end
    rows, ticks = {}, 0
    nextAt = w3.time() + 1
    handle = w3.on_ui(draw)
  elseif not on and handle then
    w3.off_ui(handle)
    handle = nil
  end
end

return M
