-- lua\lib\debugger.lua alone, with a stand-in for w3: build\lua.exe tools\vscode-witcherscript\test\unit.lua
local here = arg[0]:gsub("[^/\\]+$", "")
package.path = here .. "../../../lua/?.lua;" .. package.path

local sent, inbox, steps, lines, offs = {}, {}, {}, {}, {}
local X = 3
local SELF = {name = "geralt"}
w3 = {
  dir = function() return "C:\\Game\\bin\\x64_dx12\\tw3se" end,
  on_line = function(file, line, fn) lines[#lines + 1] = {file = file, line = line, fn = fn} return #lines end,
  off_line = function(h) offs[#offs + 1] = h return true end,
  on_tick = function(fn) end,
  classof = function(v) error("not an object") end,
  fields = function(v) error("not an object") end,
  _dbg_send = function(s) sent[#sent + 1] = s end,
  _dbg_recv = function() return table.remove(inbox, 1) end,
  _dbg_connected = function() return #inbox > 0 end,  -- the stand-in "leaves" when it has nothing more to say
  _dbg_alive = function() end,
  _dbg_step = function(m) steps[#steps + 1] = m end,
  _dbg_listen = function() return true end,
  _dbg_on_step = function(fn) STEPFN = fn end,
  _dbg_stack = function()
    return {{name = "CR4Player.OnSpawned", file = "game\\player\\r4player.ws", line = 12},
            {name = "CEntity.OnSpawned", file = "engine\\entity.ws", line = 40}}
  end,
  _dbg_frame = function(level)
    if level == 1 then
      return {{name = "x", value = X, type = "Int32"}, {name = "t", value = {a = 1, b = {c = 2}}, type = "SFoo"},
              {name = "s", value = "a\nb", type = "String"}}, SELF
    end
    return {}, nil
  end,
  _dbg_setvar = function(level, name, v) assert(level == 1 and name == "x") X = v end,
}

local D = require("lib.debugger")
assert(D.start(29170))

local fails = 0
local function last() return sent[#sent] end
local function expect(cond, what)
  if not cond then fails = fails + 1 print("FAIL " .. what .. "\n  last: " .. tostring(last())) else print("ok   " .. what) end
end
local function find(pat)
  for _, s in ipairs(sent) do if s:find(pat, 1, true) then return s end end
end
local function hit(line, info)
  for i = #lines, 1, -1 do  -- the newest watch of that line
    if lines[i].line == line then lines[i].fn(info) return end
  end
end
local INFO = {file = "game\\player\\r4player.ws", line = 12, name = "CR4Player.OnSpawned"}

D.handle("connected")
D.handle("1\thello")
expect(last():find('"game":"C:\\\\Game"', 1, true) and last():find('"seq":1', 1, true), "hello names the game folder")

D.handle("2\tbreakpoints\tGAME\\\\player\\\\r4player.ws\t12\t14?x > 5\t15!x is {x}")
expect(#lines == 3 and lines[1].file == "game\\player\\r4player.ws" and lines[2].line == 14, "three line watches")
expect(last():find('"verified":true', 1, true) and last():find('"line":15', 1, true), "breakpoints answered")

-- line 12: stop, look around, change x, step over
inbox = {"3\tstack", "4\tscopes\t1", "5\tvariables\t1", "6\tevaluate\t1\tx * 2", "7\tsetVariable\t1\tx\t42",
         "8\tvariables\t1", "9\tnext"}
sent = {}
hit(12, INFO)
expect(sent[1]:find('"event":"stopped"', 1, true) and sent[1]:find('"reason":"breakpoint"', 1, true), "stopped at 12")
expect(find('"name":"CEntity.OnSpawned"'), "stack has two calls")
expect(find('"name":"Locals"') and find('"name":"self"'), "scopes: locals and self")
local vars = find('"seq":5')
expect(vars and vars:find('"name":"x"', 1, true) and vars:find('"value":"3"', 1, true) and
       vars:find('"value":"\\"a\\\\nb\\""', 1, true), "variables with values")
expect(find('"seq":6') and find('"seq":6'):find('"value":"6"', 1, true), "evaluate x * 2")
expect(X == 42, "setVariable wrote x")
expect(find('"seq":8') and find('"seq":8'):find('"value":"42"', 1, true), "x reads 42")
expect(steps[#steps] == 2 and find('"event":"continued"'), "next: step over asked")

-- the step lands on line 13
inbox = {"10\tcontinue"}
sent = {}
STEPFN({file = INFO.file, line = 13, name = INFO.name})
expect(sent[1]:find('"reason":"step"', 1, true) and sent[1]:find('"line":13', 1, true), "stopped after the step")

-- line 14 with a condition
X = 3
inbox = {"keep"}
sent = {}
hit(14, INFO)
expect(#sent == 0 and inbox[1] == "keep", "condition false: no stop")
X = 7
inbox = {"11\tcontinue"}
hit(14, INFO)
expect(sent[1] and sent[1]:find('"reason":"breakpoint"', 1, true), "condition true: stop")

-- a log point
sent = {}
inbox = {"x"}  -- keeps the stand-in connected
hit(15, INFO)
expect(sent[1] and sent[1]:find('"text":"x is 7\\n"', 1, true), "log point writes its message")

-- variables of a nested table
inbox = {"12\tscopes\t1", "13\tvariables\t1", "14\tcontinue"}
sent = {}
hit(12, INFO)
local v13 = find('"seq":13') or ""
local tref = v13:match('"ref":([1-9]%d*)')  -- x and s have none, t has one
expect(tref and tonumber(tref) > 0, "a table variable has a reference")

-- Lua breakpoints: a chunk named like a mod's file
local chunk = load([[
local function add(a, b)
  local s = a + b
  return s * 2
end
local up = 10
function DEMO(x)
  local y = add(x, up)
  return y
end
]], "@C:\\Game\\bin\\x64_dx12\\tw3se\\mods\\demo\\init.lua")
chunk()
D.handle("30\tbreakpoints\tlua:MODS\\\\demo\\\\init.lua\t2")
expect(last():find('"seq":30', 1, true) and last():find('"line":2', 1, true), "a Lua breakpoint is set")
inbox = {"31\tstack", "32\tscopes\t1", "33\tvariables\t1", "34\tevaluate\t1\ta + b", "35\tsetVariable\t1\ta\t5",
         "36\tcontinue"}
sent = {}
local r = DEMO(3)
expect(sent[1] and sent[1]:find('"reason":"breakpoint"', 1, true) and sent[1]:find('"line":2', 1, true),
       "stopped in Lua at line 2")
local st = find('"seq":31') or ""
expect(st:find('"name":"add"', 1, true) and st:find('"name":"DEMO"', 1, true) and st:find('mods\\\\demo\\\\init.lua', 1, true),
       "the Lua stack: add, DEMO, with the file", st)
expect((find('"seq":32') or ""):find('"name":"Upvalues"', 1, true), "scopes: locals and upvalues")
local v33 = find('"seq":33') or ""
expect(v33:find('"name":"a"', 1, true) and v33:find('"value":"3"', 1, true) and v33:find('"value":"10"', 1, true),
       "a = 3, b = 10")
expect((find('"seq":34') or ""):find('"value":"13"', 1, true), "evaluate a + b in the Lua frame")
expect(r == 30, "setVariable a = 5 counts: DEMO(3) returns " .. tostring(r))

-- steps in Lua: over a call, into it, out of it
D.handle("40\tbreakpoints\tlua:mods\\\\demo\\\\init.lua\t7")
inbox = {"41\tnext"}
sent = {}
local function stop_lines() local t = {} for _, s in ipairs(sent) do local l = s:match('"event":"stopped".-"line":(%d+)') or s:match('"line":(%d+).-"event":"stopped"') if l then t[#t + 1] = tonumber(l) end end return t end
-- the step's stop comes with the inbox empty: give it a command of its own
local real_recv = w3._dbg_recv
local queue = {{"41\tnext"}, {"42\tcontinue"}}
inbox = queue[1]
w3._dbg_recv = function()
  local l = table.remove(inbox, 1)
  if not l and #queue > 1 then table.remove(queue, 1) inbox = queue[1] end
  return l
end
DEMO(1)
w3._dbg_recv = real_recv
local lines = stop_lines()
expect(lines[1] == 7 and lines[2] == 8, "step over: from 7 to 8 (" .. table.concat(lines, ",") .. ")")
queue = {{"51\tstepIn"}, {"52\tstepOut"}, {"53\tcontinue"}}
inbox = queue[1]
sent = {}
w3._dbg_recv = function()
  local l = table.remove(inbox, 1)
  if not l and #queue > 1 then table.remove(queue, 1) inbox = queue[1] end
  return l
end
DEMO(1)
w3._dbg_recv = real_recv
lines = stop_lines()
expect(lines[1] == 7 and lines[2] == 2 and lines[3] == 8, "step in to 2, out to 8 (" .. table.concat(lines, ",") .. ")")
D.handle("60\tbreakpoints\tlua:mods\\\\demo\\\\init.lua")
expect(debug.gethook() == nil, "no Lua breakpoints left: the hook is off")

-- pause while running, unknown command, disconnect
inbox = {}
D.handle("15\tpause")
expect(steps[#steps] == 1, "pause asks for the next statement")
D.handle("16\tnope")
expect(last():find('"ok":false', 1, true) and last():find("unknown command", 1, true), "unknown command")
D.handle("17\tdisconnect")
expect(#offs == 3, "disconnect takes the breakpoints away")

print(fails == 0 and "ALL OK" or (fails .. " FAILED"))
os.exit(fails == 0 and 0 or 1)
