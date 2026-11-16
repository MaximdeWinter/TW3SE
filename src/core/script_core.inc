// TW3SE's script library: natives for WitcherScript written in Lua, run at the game start before
// tw3se\lua\natives\ (lua.cpp RunEarlyNatives). A .ws mod reaches fields by name, calls functions by name, reads the
// catalog and hooks functions with an event of its own. docs/manual/script-natives.md lists the import lines.
R"TW3SELUA(
local lasterr = ""
local function def(name, fn, default)
  w3.native(name, function(...)
    local ok, r = pcall(fn, ...)
    if ok then lasterr = "" return r end
    lasterr = tostring(r):gsub("^.-:%d+: ", "")
    return default
  end)
end

-- a value as text: numbers, bools, names, enum values, structs with X Y Z W or Pitch Yaw Roll, lists joined by ","
local function totext(v, depth)
  depth = depth or 0
  local t = type(v)
  if v == nil then return "" end
  if t == "string" then return v end
  if t == "number" then return math.type(v) == "float" and string.format("%.9g", v) or tostring(v) end  -- 5, not 5.0
  if t == "boolean" then return tostring(v) end
  if t == "table" then
    if depth > 4 then return "" end
    local parts = {}
    for i, x in ipairs(v) do parts[i] = totext(x, depth + 1) end
    return table.concat(parts, ",")
  end
  if t == "userdata" then
    local ok, list = pcall(function() return v:to_table() end)
    if ok and type(list) == "table" then return totext(list, depth + 1) end
    local okx, x = pcall(function() return v.X end)
    if okx and type(x) == "number" then return string.format("%.9g %.9g %.9g %.9g", v.X, v.Y, v.Z, v.W or 1) end
    local okp, p = pcall(function() return v.Pitch end)
    if okp and type(p) == "number" then return string.format("%.9g %.9g %.9g", v.Pitch, v.Yaw, v.Roll) end
  end
  return tostring(v)
end

-- text for a parameter or field of a given type (kind: 11 fundamental, 1 enum, 9 bitfield, 2 struct)
local function fromtext(s, ty, kind, what)
  if kind == 11 then
    if ty == "Bool" then return s == "true" or s == "1" end
    local n = tonumber(s)
    if n == nil then error(what .. ": a number is needed, got '" .. s .. "'") end
    if ty == "Float" or ty == "Double" then return n end
    return math.tointeger(n) or error(what .. ": a whole number is needed, got '" .. s .. "'")
  end
  if kind == 1 or kind == 9 then return math.tointeger(tonumber(s)) or s end
  if kind == 2 then
    local n = {}
    for x in s:gmatch("[^%s,]+") do n[#n + 1] = tonumber(x) or error(what .. ": numbers are needed for a " .. ty) end
    if ty == "Vector" then return {X = n[1] or 0, Y = n[2] or 0, Z = n[3] or 0, W = n[4] or 1} end
    if ty == "EulerAngles" then return {Pitch = n[1] or 0, Yaw = n[2] or 0, Roll = n[3] or 0} end
    error(what .. ": a " .. ty .. " cannot come from text")
  end
  return s
end

local function need(o, what)
  if o == nil then error(what .. ": the object is NULL") end
  return o
end

local function field(o, f)
  need(o, f)
  local m = w3._member(o, f)
  if m ~= "field" then error(w3.classof(o) .. " has no field " .. f) end
  return o[f]
end

-- fields by name
def("TW3SE_GetInt", function(o, f)
  local v = field(o, f)
  if type(v) == "boolean" then return v and 1 or 0 end
  if type(v) == "number" then return math.tointeger(v) or math.floor(v) end
  error(f .. " is no number")
end, 0)
def("TW3SE_GetFloat", function(o, f)
  local v = field(o, f)
  if type(v) == "number" then return v end
  error(f .. " is no number")
end, 0)
def("TW3SE_GetBool", function(o, f)
  local v = field(o, f)
  if type(v) == "number" then return v ~= 0 end
  return v == true
end, false)
def("TW3SE_GetString", function(o, f) return totext(field(o, f)) end, "")
def("TW3SE_GetObject", function(o, f)
  local v = field(o, f)
  if type(v) ~= "userdata" or tostring(v):find("(struct)", 1, true) then error(f .. " is no object") end
  return v
end, nil)
local function set(o, f, v)
  need(o, f)
  local m, ty, kind = w3._member(o, f)
  if m ~= "field" then error(w3.classof(o) .. " has no field " .. f) end
  if type(v) == "string" then v = fromtext(v, ty, kind, f) end
  o[f] = v
  return true
end
def("TW3SE_SetInt", set, false)
def("TW3SE_SetFloat", set, false)
def("TW3SE_SetBool", set, false)
def("TW3SE_SetString", set, false)
def("TW3SE_SetObject", set, false)
def("TW3SE_HasField", function(o, f) return o ~= nil and w3._member(o, f) == "field" end, false)
def("TW3SE_HasMethod", function(o, f) return o ~= nil and w3._member(o, f) == "method" end, false)

-- calls by name: o NULL calls a global function; the text arguments become the parameters' types
local function call(o, fn, ...)
  local a = {...}
  while #a > 0 and a[#a] == "" do a[#a] = nil end
  local ps = w3._params(o, fn)
  if not ps then error((o and (w3.classof(o) .. " has no method ") or "no function ") .. fn) end
  for i = 1, #a do
    local p = ps[i]
    if p then a[i] = fromtext(a[i], p.type, p.kind, "argument " .. p.name) end
  end
  if o == nil then return w3.call(fn, table.unpack(a, 1, #a)) end
  return o[fn](o, table.unpack(a, 1, #a))
end
def("TW3SE_Call", function(o, fn, ...) return totext(call(o, fn, ...)) end, "")
def("TW3SE_CallObject", function(o, fn, ...)
  local v = call(o, fn, ...)
  if type(v) ~= "userdata" or tostring(v):find("(struct)", 1, true) then error(fn .. " returned no object") end
  return v
end, nil)

-- the catalog
def("TW3SE_ClassOf", function(o) return w3.classof(need(o, "TW3SE_ClassOf")) end, "")
def("TW3SE_ClassParent", function(c) return w3.class(c).parent or "" end, "")
def("TW3SE_Fields", function(c)
  local names = {}
  for n, f in pairs(w3.class(c).fields) do names[#names + 1] = n .. ":" .. f.type end
  table.sort(names)
  return table.concat(names, ",")
end, "")
def("TW3SE_Methods", function(c)
  local seen, names = {}, {}
  for _, n in ipairs(w3.class(c).methods) do
    if not seen[n] then seen[n] = true names[#names + 1] = n end
  end
  table.sort(names)
  return table.concat(names, ",")
end, "")
def("TW3SE_EnumValue", function(e, n)
  local v = w3.enum(e)[n]
  if v == nil then error("no value " .. n .. " in " .. e) end
  return v
end, 0)
def("TW3SE_EnumName", function(e, v)
  for n, x in pairs(w3.enum(e)) do if x == v then return n end end
  error("no value " .. tostring(v) .. " in " .. e)
end, "")

-- every object of a class: TW3SE_AllObjects takes a snapshot and gives its size, TW3SE_ObjectAt(i) one of it (from 0)
local snapshot = {}
def("TW3SE_AllObjects", function(c)
  snapshot = w3.all(c)
  return #snapshot
end, 0)
def("TW3SE_ObjectAt", function(i)
  local o = snapshot[i + 1]
  if o == nil then error("no object " .. tostring(i) .. " (TW3SE_AllObjects found " .. #snapshot .. ")") end
  return o
end, nil)

-- hooks: after each call of the function, listener.event(self) runs (self: the object it ran on, NULL for a global)
def("TW3SE_HookEvent", function(c, fn, listener, event)
  need(listener, "TW3SE_HookEvent")
  if c == "" then c = nil end
  return w3.hook(c, fn, function(self, args, orig)
    orig()
    local ok, e = pcall(function() listener[event](listener, self) end)
    if not ok then lasterr = tostring(e) end
  end)
end, 0)
def("TW3SE_Unhook", function(id) return w3.unhook(id) end, false)
def("TW3SE_LastError", function() return lasterr end, "")

-- 1.2: values that go with the save (w3.saved "script.<mod>"), and web requests whose answer comes back as a message
-- on `channel` (TW3SE_Receive): the status, a line break, the body (status 0 and the reason when none came)
def("TW3SE_SavedSet", function(mod, key, value) w3.saved("script." .. mod)[key] = value return true end, false)
def("TW3SE_SavedGet", function(mod, key)
  local v = w3.saved("script." .. mod)[key]
  return v == nil and "" or tostring(v)
end, "")
def("TW3SE_SavedHas", function(mod, key) return w3.saved("script." .. mod)[key] ~= nil end, false)
def("TW3SE_SavedRemove", function(mod, key) w3.saved("script." .. mod)[key] = nil return true end, false)
def("TW3SE_HttpRequest", function(method, url, body, channel)
  if url == "" or channel == "" then error("a url and a channel are needed") end
  w3.http.request({method = method ~= "" and method or "GET", url = url, body = body ~= "" and body or nil},
    function(status, text, headers, err) w3.send(channel, tostring(status) .. "\n" .. (text or err or "")) end)
  return true
end, false)
)TW3SELUA"
