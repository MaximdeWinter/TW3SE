-- The WitcherScript and Lua debugger: breakpoints, steps, the call stack and the variables of each call, for
-- VS Code (tools\vscode-witcherscript) or any client of the same protocol. Started by `debugger = on` in
-- tw3se.config (port `debugger_port`, 29170), or from Lua: require('lib.debugger').start(port).
--
-- The connection is one client on 127.0.0.1 (debugger.cpp). The client sends lines "seq<TAB>command<TAB>arg...",
-- where a tab, a newline and a backslash in an argument are written \t, \n and \\. The game answers each with one JSON
-- line {"seq": n, "ok": true, "body": {...}} or {"seq": n, "ok": false, "error": "..."}, and sends events as JSON
-- lines {"event": "stopped" | "continued" | "output", ...}.
--
-- A breakpoint is w3.on_line on the file and line. While the game stands at one, the main thread runs the loop in
-- halt() and nothing else: the game is frozen and answers the client from there. Expressions (watch, hover,
-- conditions, the debug console) are Lua, with the call's variables and `self` in scope.
local M = {}

-- JSON ----------------------------------------------------------------------------------------------------------
local ARRAY = {}
local function arr(t) return setmetatable(t or {}, ARRAY) end
local esc = {['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t'}
local function json(v)
  local t = type(v)
  if t == "nil" then return "null" end
  if t == "boolean" then return tostring(v) end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if math.type(v) == "integer" then return tostring(v) end
    return string.format("%.17g", v)
  end
  if t == "string" then
    return '"' .. v:gsub('[%c"\\]', function(c) return esc[c] or string.format("\\u%04x", c:byte()) end) .. '"'
  end
  if t == "table" then
    local out = {}
    if getmetatable(v) == ARRAY then
      for i = 1, #v do out[i] = json(v[i]) end
      return "[" .. table.concat(out, ",") .. "]"
    end
    for k, x in pairs(v) do out[#out + 1] = json(tostring(k)) .. ":" .. json(x) end
    return "{" .. table.concat(out, ",") .. "}"
  end
  return json(tostring(v))
end
M.json = json

local function send(t) w3._dbg_send(json(t)) end
local function event(name, body)
  body = body or {}
  body.event = name
  send(body)
end
local function output(text, category) event("output", {text = text, category = category or "console"}) end

-- values ---------------------------------------------------------------------------------------------------------
local refs, nextRef = {}, 0
local function ref(container)
  nextRef = nextRef + 1
  refs[nextRef] = container
  return nextRef
end

local function is_object(v) return type(v) == "userdata" and pcall(w3.classof, v) end
local function is_array(v)
  local mt = type(v) == "userdata" and getmetatable(v)
  return type(mt) == "table" and mt.__name == "tw3se.array"
end
local function is_struct(v) return type(v) == "userdata" and tostring(v):find("(struct)", 1, true) ~= nil end

-- a value as the client shows it: text, type and a reference when it has children
local function describe(v, typeName)
  local t = type(v)
  if t == "string" then
    return {value = '"' .. v:gsub('[\r\n\t]', {['\r'] = '\\r', ['\n'] = '\\n', ['\t'] = '\\t'}) .. '"',
            type = typeName or "string", ref = 0}
  end
  if t == "number" or t == "boolean" or t == "nil" then
    return {value = tostring(v), type = typeName or t, ref = 0}
  end
  if is_array(v) then
    return {value = (typeName or "array") .. " [" .. #v .. "]", type = typeName or "array",
            ref = #v > 0 and ref({kind = "array", v = v}) or 0}
  end
  if is_object(v) or is_struct(v) then
    local text = tostring(v)
    if is_object(v) then
      local ok, name = pcall(function() return v.name end)
      if ok and type(name) == "string" and name ~= "" then text = text .. ' "' .. name .. '"' end
    end
    return {value = text, type = typeName or tostring(v), ref = ref({kind = "fields", v = v})}
  end
  if t == "table" then
    local n = 0
    for _ in pairs(v) do n = n + 1 end
    return {value = "{" .. n .. "}", type = typeName or "table", ref = n > 0 and ref({kind = "table", v = v}) or 0}
  end
  return {value = tostring(v), type = typeName or t, ref = 0}
end

local function item(name, v, typeName)
  local d = describe(v, typeName)
  d.name = tostring(name)
  return d
end

local MAX_CHILDREN = 500
local lua_vars, lua_upvalues  -- (Lua code, below)
local halted                  -- the stop the game stands at, nil while it runs

local function children(c)
  local out = arr()
  if c.kind == "frame" then
    local vars = w3._dbg_frame(c.level) or {}
    for _, x in ipairs(vars) do out[#out + 1] = item(x.name, x.value, x.type) end
  elseif c.kind == "lua_locals" or c.kind == "lua_upvalues" then
    local list = c.kind == "lua_locals" and lua_vars(c.func) or lua_upvalues(c.func)
    for _, x in ipairs(list or {}) do out[#out + 1] = item(x.name, x.value) end
  elseif c.kind == "array" then
    for i = 1, math.min(#c.v, MAX_CHILDREN) do out[#out + 1] = item("[" .. i .. "]", c.v[i]) end
    if #c.v > MAX_CHILDREN then out[#out + 1] = {name = "...", value = (#c.v - MAX_CHILDREN) .. " more", ref = 0} end
  elseif c.kind == "fields" then
    local ok, fields = pcall(w3.fields, c.v)
    local names = {}
    for name in pairs(ok and fields or {}) do names[#names + 1] = name end
    table.sort(names)
    for _, name in ipairs(names) do
      local okv, v = pcall(function() return c.v[name] end)
      out[#out + 1] = okv and item(name, v) or {name = name, value = "?", ref = 0}
    end
  elseif c.kind == "table" then
    local keys = {}
    for k in pairs(c.v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for i = 1, math.min(#keys, MAX_CHILDREN) do out[#out + 1] = item(keys[i], c.v[keys[i]]) end
  end
  return out
end

-- Lua code: a halt in Lua keeps the calls of the moment (their functions); their locals are read and written
-- through the innermost call of each function still on the stack below the halt loop
local SELF = debug.getinfo(1, "S").source

function lua_vars(func, setName, setValue)  -- {{name, value}} of the call's locals, nil when it is gone
  for l = 2, 250 do
    local i = debug.getinfo(l, "f")
    if not i then return nil end
    if i.func == func then
      local out, k = {}, 1
      while true do
        local n, v = debug.getlocal(l, k)
        if not n then break end
        if n:sub(1, 1) ~= "(" then
          if n == setName then debug.setlocal(l, k, setValue) v = setValue end
          out[#out + 1] = {name = n, value = v}
        end
        k = k + 1
      end
      return out
    end
  end
end
function lua_upvalues(func, setName, setValue)
  local out, k = {}, 1
  while true do
    local n, v = debug.getupvalue(func, k)
    if not n then break end
    if n == setName then debug.setupvalue(func, k, setValue) v = setValue end
    if n ~= "_ENV" then out[#out + 1] = {name = n, value = v} end
    k = k + 1
  end
  return out
end

-- expressions in a call's scope: its variables by name, `self`, then everything global
local function scope(level)
  local vars, self = {}, nil
  local frame = halted and halted.lua and halted.lua[level or 1]
  if frame then
    if frame.func then
      for _, x in ipairs(lua_upvalues(frame.func)) do vars[x.name] = x.value end
      for _, x in ipairs(lua_vars(frame.func) or {}) do vars[x.name] = x.value end
      self = vars.self
    end
  else
    local list, obj = w3._dbg_frame(level or 1)
    for _, x in ipairs(list or {}) do vars[x.name] = x.value end
    self = obj
  end
  return setmetatable({self = self}, {__index = function(_, k)
    if vars[k] ~= nil then return vars[k] end
    return _G[k]
  end})
end

local function evaluate(expr, level)
  local env = scope(level)
  local f, err = load("return " .. expr, "=expression", "t", env)
  if not f then f, err = load(expr, "=expression", "t", env) end
  if not f then error(err, 0) end
  return f()
end

-- breakpoints ------------------------------------------------------------------------------------------------------
local bps = {}  -- file -> {line -> {handle, cond, log}}
local stepReason = "step"

local function interpolate(text, info)
  return (text:gsub("{(.-)}", function(expr)
    local ok, v = pcall(evaluate, expr, 1)
    return ok and tostring(v) or ("<" .. tostring(v) .. ">")
  end))
end

local handle  -- forward

local function halt(info, reason, lua)
  halted = {info = info, reason = reason, lua = lua and lua.frames, depth = lua and lua.depth}
  refs, nextRef = {}, 0
  event("stopped", {reason = reason, file = info.file, line = info.line, name = info.name})
  while halted do
    w3._dbg_alive()
    local line = w3._dbg_recv(100)
    if line then handle(line) end
    if halted and not w3._dbg_connected() then halted = nil end
  end
  refs, nextRef = {}, 0
end
M.halt = halt

local function on_line(file, line)
  return function(info)
    local b = bps[file] and bps[file][line]
    if not b or not w3._dbg_connected() then return end
    if b.cond then
      local ok, v = pcall(evaluate, b.cond, 1)
      if not ok then output("condition " .. b.cond .. ": " .. tostring(v) .. "\n", "stderr") return end
      if not v then return end
    end
    if b.log then output(interpolate(b.log, info) .. "\n") return end
    halt(info, "breakpoint")
  end
end

local function set_breakpoints(file, specs)
  for line, b in pairs(bps[file] or {}) do w3.off_line(b.handle) end
  bps[file] = {}
  local out = arr()
  for _, spec in ipairs(specs) do
    -- "12", "12?cond" (a condition), "12!message" (a log point, {expr} filled in)
    local line, kind, rest = spec:match("^(%d+)([?!]?)(.*)$")
    line = tonumber(line)
    if line then
      local b = {cond = kind == "?" and rest or nil, log = kind == "!" and rest or nil}
      b.handle = w3.on_line(file, line, on_line(file, line))
      bps[file][line] = b
      out[#out + 1] = {line = line, verified = true}
    end
  end
  return out
end

-- Lua breakpoints: a line hook (debug.sethook) while any is set or a Lua step is asked for. A file is known by the
-- end of its path below tw3se (mods\mymod\init.lua); the chunk names of mods and require'd files are their full
-- paths. Code in w3.run sequences (coroutines) runs without the hook.
local luabps, luacount, luastep, srccache = {}, 0, nil, {}
local lua_hook

local function rehook()
  if luacount > 0 or luastep then debug.sethook(lua_hook, "l") else debug.sethook() end
end

local function lua_lines(source)  -- the breakpoints of a chunk, or nil
  local t = srccache[source]
  if t == nil then
    t = false
    local s = source:sub(2):lower():gsub("/", "\\")
    for key, lines in pairs(luabps) do
      if s:sub(-#key) == key and (#s == #key or s:sub(-#key - 1, -#key - 1) == "\\") then t = lines break end
    end
    srccache[source] = t
  end
  return t or nil
end

local function lua_frames(from)  -- the Lua calls from level `from` of the caller outwards
  local frames, l = arr(), from + 1
  while true do
    local i = debug.getinfo(l, "Snlf")
    if not i then break end
    local path = i.source:sub(1, 1) == "@" and i.source:sub(2) or nil
    frames[#frames + 1] = {func = i.func, line = i.currentline, path = path,
                           name = i.name or (i.what == "main" and "(main chunk)") or
                                  ("function " .. i.short_src .. ":" .. tostring(i.linedefined))}
    l = l + 1
  end
  return frames
end

local function lua_condition(b, func)  -- true when the breakpoint stops (a log point writes and goes on)
  local function env()
    local vars = {}
    for _, x in ipairs(lua_upvalues(func)) do vars[x.name] = x.value end
    for _, x in ipairs(lua_vars(func) or {}) do vars[x.name] = x.value end
    return setmetatable({}, {__index = function(_, k) if vars[k] ~= nil then return vars[k] end return _G[k] end})
  end
  if b.cond then
    local f = load("return " .. b.cond, "=condition", "t", env())
    local ok, v = pcall(f or error)
    if not ok then output("condition " .. b.cond .. ": " .. tostring(v) .. "\n", "stderr") return false end
    if not v then return false end
  end
  if b.log then
    local e = env()
    output(b.log:gsub("{(.-)}", function(x)
      local f = load("return " .. x, "=log", "t", e)
      local ok, v = pcall(f or error)
      return ok and tostring(v) or ("<" .. tostring(v) .. ">")
    end) .. "\n")
    return false
  end
  return true
end

lua_hook = function(_, line)
  if halted then return end
  local info = debug.getinfo(2, "Sf")
  local src = info.source
  if src == SELF or src:sub(1, 1) ~= "@" then return end
  local reason
  if luastep then
    local d = 0
    while debug.getinfo(2 + d, "l") do d = d + 1 end
    if luastep.mode == 1 or (luastep.mode == 2 and d <= luastep.depth) or (luastep.mode == 3 and d < luastep.depth) then
      reason = luastep.reason or "step"
    end
  end
  if not reason then
    local lines = lua_lines(src)
    local b = lines and lines[line]
    if b and w3._dbg_connected() and lua_condition(b, info.func) then reason = "breakpoint" end
  end
  if not reason then return end
  luastep = nil
  rehook()
  local frames = lua_frames(2)
  local d = #frames
  halt({file = frames[1].path or src, line = line, name = frames[1].name}, reason, {frames = frames, depth = d})
end

local function set_lua_breakpoints(key, specs)
  if luabps[key] then luacount = luacount - 1 end
  luabps[key] = {}
  srccache = {}
  local out = arr()
  for _, spec in ipairs(specs) do
    local line, kind, rest = spec:match("^(%d+)([?!]?)(.*)$")
    line = tonumber(line)
    if line then
      luabps[key][line] = {cond = kind == "?" and rest or nil, log = kind == "!" and rest or nil}
      out[#out + 1] = {line = line, verified = true}
    end
  end
  if #out > 0 then luacount = luacount + 1 else luabps[key] = nil end
  rehook()
  return out
end

local function clear_all()
  for file, lines in pairs(bps) do
    for _, b in pairs(lines) do w3.off_line(b.handle) end
  end
  bps = {}
  w3._dbg_step(0)
  luabps, luacount, luastep, srccache = {}, 0, nil, {}
  rehook()
  halted = nil
end

-- commands ---------------------------------------------------------------------------------------------------------
local function unescape(s)
  return (s:gsub("\\(.)", {t = "\t", n = "\n", ["\\"] = "\\"}))
end

local function resume(mode, reason)
  stepReason = reason
  if halted and halted.lua then
    luastep = mode and {mode = mode, depth = halted.depth, reason = reason} or nil
    rehook()
  elseif mode then
    w3._dbg_step(mode)
  end
  halted = nil
  event("continued")
end

local function game_root()
  return (w3.dir():gsub("\\bin\\x64_dx12\\tw3se$", ""))
end

local commands = {}
function commands.hello() return {version = 1, game = game_root()} end
function commands.breakpoints(file, ...)
  file = file:lower()
  if file:sub(1, 4) == "lua:" then return {breakpoints = set_lua_breakpoints(file:sub(5), {...})} end
  return {breakpoints = set_breakpoints(file, {...})}
end
function commands.continue() resume(nil) return {} end
function commands.next() resume(2, "step") return {} end
function commands.stepIn() resume(1, "step") return {} end
function commands.stepOut() resume(3, "step") return {} end
function commands.pause()
  if not halted then
    stepReason = "pause"
    w3._dbg_step(1)
    luastep = {mode = 1, depth = 0, reason = "pause"}
    rehook()
  end
  return {}
end
function commands.stack()
  local frames = arr()
  if halted and halted.lua then
    for i, f in ipairs(halted.lua) do frames[i] = {id = i, name = f.name, file = "", path = f.path, line = f.line} end
    return {frames = frames}
  end
  for i, f in ipairs(w3._dbg_stack()) do
    frames[#frames + 1] = {id = i, name = f.name, file = f.file, line = f.line}
  end
  return {frames = frames}
end
function commands.scopes(level)
  level = tonumber(level) or 1
  local lf = halted and halted.lua and halted.lua[level]
  if lf then
    return {scopes = arr({{name = "Locals", ref = ref({kind = "lua_locals", func = lf.func})},
                          {name = "Upvalues", ref = ref({kind = "lua_upvalues", func = lf.func})}})}
  end
  local _, self = w3._dbg_frame(level)
  local scopes = arr({{name = "Locals", ref = ref({kind = "frame", level = level})}})
  if self then scopes[2] = {name = "self", ref = ref({kind = "fields", v = self})} end
  return {scopes = scopes}
end
function commands.variables(r)
  local c = refs[tonumber(r)]
  if not c then error("the variables are gone (the game went on)", 0) end
  return {variables = children(c)}
end
function commands.evaluate(level, expr)
  local d = describe(evaluate(expr, tonumber(level) or 1))
  return d
end
function commands.setVariable(r, name, expr)
  local c = refs[tonumber(r)]
  if not c then error("the variables are gone (the game went on)", 0) end
  local level = c.kind == "frame" and c.level or 1
  local v = evaluate(expr, level)
  if c.kind == "frame" then
    w3._dbg_setvar(c.level, name, v)
  elseif c.kind == "lua_locals" then
    lua_vars(c.func, name, v)
  elseif c.kind == "lua_upvalues" then
    lua_upvalues(c.func, name, v)
  elseif c.kind == "array" then
    c.v[tonumber(name:match("%d+"))] = v
  else
    c.v[name] = v
  end
  return describe(v)
end
function commands.disconnect() clear_all() return {} end

function handle(line)
  if line == "connected" or line == "disconnected" then
    clear_all()
    return
  end
  local parts = {}
  for field in (line .. "\t"):gmatch("(.-)\t") do parts[#parts + 1] = unescape(field) end
  local seq, name = tonumber(parts[1]), parts[2]
  local fn = commands[name or ""]
  if not fn then
    send({seq = seq, ok = false, error = "unknown command " .. tostring(name)})
    return
  end
  local ok, body = pcall(fn, table.unpack(parts, 3))
  if ok then send({seq = seq, ok = true, body = body or {}})
  else send({seq = seq, ok = false, error = tostring(body)}) end
end
M.handle = handle

local started
function M.start(port)
  port = port or 29170
  if started then return true end
  if not w3._dbg_listen(port) then return false, "port " .. port .. " is taken" end
  started = true
  w3._dbg_on_step(function(info) halt(info, stepReason) end)
  w3.on_tick(function()
    for _ = 1, 64 do
      local line = w3._dbg_recv(0)
      if not line then break end
      handle(line)
    end
  end)
  return true
end

return M
