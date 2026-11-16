-- The inspector of the in-game UI (the console's "Inspector" box): any object with its fields, live. Numbers, bools,
-- strings and enums can be changed in place, object fields open on a click, methods without arguments can be called.
-- Built on w3.ui like any Lua mod window. From the console: require('lib.inspector').look('w3.near(10)[1]')
local M = {}
local state = { expr = "w3.player()", stack = {}, filter = "", err = nil, results = {} }
local handle

local RED = 0xFF8070FF
local GREY = 0xA0A0A0FF

local function look(expr)
  local f, err = load("return " .. expr, "=inspect")
  if not f then state.err = err return end
  local ok, v = pcall(f)
  if not ok then state.err = tostring(v) return end
  if not pcall(w3.classof, v) then state.err = "not an object: " .. tostring(v) return end
  state.err, state.results = nil, {}
  state.stack = { { obj = v, label = expr } }
end

local function push(v, label)
  state.stack[#state.stack + 1] = { obj = v, label = label }
  state.results = {}
end

local function array_cell(v, name)
  w3.ui.tree(tostring(v), function()
    for i = 1, math.min(#v, 200) do
      local e = v[i]
      w3.ui.id(i, function()
        if pcall(w3.classof, e) then
          if w3.ui.button(i .. ": " .. tostring(e)) then push(e, name .. "[" .. i .. "]") end
        else
          w3.ui.text(i .. ": " .. tostring(e))
        end
      end)
    end
    if #v > 200 then w3.ui.text("... " .. (#v - 200) .. " more", GREY) end
  end)
end

local function value_cell(obj, name)
  local ok, v = pcall(function() return obj[name] end)
  if not ok then w3.ui.text("?", RED) return end
  local t = type(v)
  w3.ui.width(-1)
  if t == "number" then
    local changed, n = w3.ui.input_number("##v", v)
    if changed then pcall(function() obj[name] = n end) end
  elseif t == "boolean" then
    local changed, b = w3.ui.checkbox("##v", v)
    if changed then pcall(function() obj[name] = b end) end
  elseif t == "string" then
    local changed, s = w3.ui.input_text("##v", v, true)
    if changed then pcall(function() obj[name] = s end) end
  elseif t == "userdata" then
    if pcall(w3.classof, v) then
      if w3.ui.button(tostring(v)) then push(v, name) end
    else
      array_cell(v, name)
    end
  else
    w3.ui.text(tostring(v), GREY)
  end
end

local function breadcrumb()
  for i, s in ipairs(state.stack) do
    if i > 1 then w3.ui.same_line() w3.ui.text(">") w3.ui.same_line() end
    w3.ui.id("crumb" .. i, function()
      if w3.ui.button(s.label) then
        for j = #state.stack, i + 1, -1 do state.stack[j] = nil end
        state.results = {}
      end
    end)
  end
end

local function methods(obj, cls)
  local ok, info = pcall(w3.class, cls)
  if not ok or not info.methods then return end
  w3.ui.collapsing("Methods of " .. cls .. " (" .. #info.methods .. ")", function()
    w3.ui.text("Call runs the method without arguments.", GREY)
    for _, m in ipairs(info.methods) do
      w3.ui.id("m" .. m, function()
        if w3.ui.button("Call") then
          local okc, r = pcall(function() return obj[m](obj) end)
          state.results[m] = (okc and "" or "error: ") .. tostring(r)
        end
        w3.ui.same_line()
        w3.ui.text(m .. (state.results[m] and ("  ->  " .. state.results[m]) or ""))
      end)
    end
  end)
end

-- Pick: the entity nearest to the cursor on the screen (within 80 px, up to 60 m away) gets a ring; a click outside
-- the windows opens it, Esc ends picking. Its world position goes through w3.camera.to_screen.
local function name_of(e)
  local ok, n = pcall(function() return e:GetDisplayName() end)
  if ok and type(n) == "string" and n ~= "" then return n end
  return w3.classof(e)
end
local function pick()
  local mx, my = w3.ui.mouse()
  local sw, sh = w3.ui.screen()
  local best, bx, by, bd = nil, 0, 0, 80 * 80
  for _, e in ipairs(w3.near(60) or {}) do
    local ok, x, y = pcall(function() return w3.camera.to_screen(e:GetWorldPosition()) end)
    if ok and x and y then
      local px, py = x * sw, y * sh
      local d = (px - mx) ^ 2 + (py - my) ^ 2
      if d < bd then best, bx, by, bd = e, px, py, d end
    end
  end
  w3.ui.draw.layer("front", function()
    w3.ui.draw.text(mx + 16, my + 16, 0xFFE08AFF, best and name_of(best) or "Pick: click an object (Esc: stop)")
    if best then w3.ui.draw.circle(bx, by, 14, 0xFFE08AFF, 2) end
  end)
  if w3.ui.pressed("Escape") then state.picking = false return end
  local overWindow = w3.ui.wants()
  if best and w3.ui.mouse_clicked(1) and not overWindow then
    state.picking = false
    state.err, state.results = nil, {}
    state.stack = { { obj = best, label = name_of(best) } }
  end
end

local function draw()
  if state.picking then pcall(pick) end
  local _, keep = w3.ui.window("TW3SE Inspector", function()
    w3.ui.width(420)
    local changed, e = w3.ui.input_text("Object (Lua)", state.expr, true)
    if changed then state.expr = e look(e) end
    w3.ui.same_line()
    if w3.ui.button("Look") then look(state.expr) end
    w3.ui.same_line()
    if w3.ui.button(state.picking and "Picking..." or "Pick") then state.picking = not state.picking end
    w3.ui.tooltip("Opens the object under the mouse in the world")
    if state.err then w3.ui.text(state.err, RED) end
    local cur = state.stack[#state.stack]
    if not cur then return end
    breadcrumb()
    if not w3.valid(cur.obj) then w3.ui.text("The object is gone (destroyed in the game).", RED) return end
    local cls = w3.classof(cur.obj)
    w3.ui.text(cls, GREY)
    methods(cur.obj, cls)
    local fc, f = w3.ui.input_text("Filter", state.filter)
    if fc then state.filter = f end
    local fields = w3.fields(cur.obj)
    local names, lf = {}, state.filter:lower()
    for n in pairs(fields) do
      if lf == "" or n:lower():find(lf, 1, true) then names[#names + 1] = n end
    end
    table.sort(names)
    w3.ui.child("fields", 0, function()
      w3.ui.table("f", { "Field", "Type", "Value" }, function()
        for _, n in ipairs(names) do
          w3.ui.row()
          w3.ui.col() w3.ui.text(n)
          w3.ui.col() w3.ui.text(fields[n].type, GREY)
          w3.ui.col() w3.ui.id(n, function() value_cell(cur.obj, n) end)
        end
      end)
    end)
  end, 760, 640, true)
  if keep == false then  -- its X: the console's Inspector box goes off with it
    M.show(false)
    if w3._ui_flag then w3._ui_flag("inspector", false) end
  end
end

function M.show(on)
  if on and not handle then
    if #state.stack == 0 then look(state.expr) end
    handle = w3.on_ui(draw)
  elseif not on and handle then
    w3.off_ui(handle)
    handle = nil
  end
end

function M.look(expr)
  state.expr = expr
  look(expr)
  M.show(true)
end

return M
