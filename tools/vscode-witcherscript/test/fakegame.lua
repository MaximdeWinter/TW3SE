-- The game for the adapter test: TW3SE's real lua\lib\debugger.lua over stdin/stdout, with a stand-in for w3.
-- lua.exe fakegame.lua <repository> <game folder>. Lines from the test: "@hit <line>" runs the watched line,
-- "@step <line>" the statement a step asked for; every other line is a command from the client.
local repo, game = arg[1], arg[2]
package.path = repo .. "/lua/?.lua;" .. package.path

local X = 3
local watches, stepFn = {}, nil
w3 = {
  dir = function() return game .. "\\bin\\x64_dx12\\tw3se" end,
  on_line = function(file, line, fn) watches[#watches + 1] = {file = file, line = line, fn = fn} return #watches end,
  off_line = function(h) watches[h] = {} return true end,
  on_tick = function() end,
  classof = function() error("not an object") end,
  fields = function() error("not an object") end,
  _dbg_send = function(s) io.write(s, "\n") io.flush() end,
  _dbg_recv = function() return io.read("l") end,
  _dbg_connected = function() return true end,
  _dbg_alive = function() end,
  _dbg_step = function(m) io.stderr:write("step " .. m .. "\n") end,
  _dbg_listen = function() return true end,
  _dbg_on_step = function(fn) stepFn = fn end,
  _dbg_stack = function()
    return {{name = "CR4Player.OnSpawned", file = "game\\player\\r4player.ws", line = 12},
            {name = "CEntity.Spawn", file = "", line = 0}}
  end,
  _dbg_frame = function(level)
    if level == 1 then return {{name = "x", value = X, type = "Int32"}}, {name = "geralt"} end
    return {}, nil
  end,
  _dbg_setvar = function(_, _, v) X = v end,
}

local D = require("lib.debugger")
D.start(0)
-- a Lua mod of the game folder: "@luacall" runs DEMO(3)
local modfile = game .. "\\bin\\x64_dx12\\tw3se\\mods\\demo\\init.lua"
local f = io.open(modfile, "rb")
if f then
  local code = f:read("a")
  f:close()
  load(code, "@" .. modfile)()
end
local INFO = {file = "game\\player\\r4player.ws", name = "CR4Player.OnSpawned"}
for line in io.lines() do
  local cmd, n = line:match("^@(%a+) (%d+)$")
  if line == "@luacall" then
    io.stderr:write("DEMO returned " .. tostring(DEMO(3)) .. "\n")
  elseif cmd == "hit" then
    for _, w in ipairs(watches) do
      if w.line == tonumber(n) and w.fn then w.fn({file = INFO.file, line = tonumber(n), name = INFO.name}) end
    end
  elseif cmd == "step" then
    stepFn({file = INFO.file, line = tonumber(n), name = INFO.name})
  else
    D.handle(line)
  end
end
