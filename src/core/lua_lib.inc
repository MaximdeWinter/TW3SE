// TW3SE's Lua library for the game's subsystems: thin tables over the game's own script functions, run after the
// prelude (lua.cpp State). docs/LUA.md has the reference.
R"TW3SELIB(
local function game() return w3.global("theGame") end
local function player() return w3.player() or error("no player (main menu)") end
local function enum_name(enumName, short)  -- "vitality" -> "BCS_Vitality"; a full name passes through
  local e = w3.enum(enumName)
  if e[short] then return short end
  local want = tostring(short):lower():gsub("[_%s]", "")
  for n in pairs(e) do
    local plain = n:gsub("^%u+_", ""):lower():gsub("_", "")
    if plain == want then return n end
  end
  error("no value " .. tostring(short) .. " in " .. enumName)
end

-- Time of day and weather -------------------------------------------------------------------------------
local clock = w3.time  -- w3.time() stays the real clock (seconds); w3.time.get() and the rest are the game's time
w3.time = setmetatable({}, {__call = function(_, ...) return clock(...) end})
function w3.time.get()
  local t = game():GetGameTime()
  return {day = w3.call("GameTimeDays", t), hour = w3.call("GameTimeHours", t), minute = w3.call("GameTimeMinutes", t),
          second = w3.call("GameTimeSeconds", t), seconds = w3.call("GameTimeToSeconds", t)}
end
function w3.time.set(hour, minute, day)
  local now = w3.time.get()
  local t = w3.call("GameTimeCreate", day or now.day, hour or now.hour, minute or 0, 0)
  game():SetGameTime(t, true)
end
function w3.time.add(hours, minutes)  -- forward in time (as meditation does)
  local now = w3.time.get()
  local total = now.seconds + math.floor(((hours or 0) * 60 + (minutes or 0)) * 60)
  local t = w3.call("GameTimeCreate", total // 86400, total % 86400 // 3600, total % 3600 // 60, total % 60)
  game():SetGameTime(t, true)
end
function w3.time.speed(hoursPerMinute)  -- game hours per real minute (the game's default is 0.25)
  if hoursPerMinute ~= nil then
    if type(hoursPerMinute) ~= "number" or not (hoursPerMinute >= 0 and hoursPerMinute <= 1000) then
      error("game hours per real minute: 0 to 1000", 2)
    end
    game():SetHoursPerMinute(hoursPerMinute)
  end
  return game():GetHoursPerMinute()
end

w3.weather = {}
function w3.weather.get() return w3.call("GetWeatherConditionName") end
function w3.weather.set(name, blendSeconds) return w3.call("RequestWeatherChangeTo", name, blendSeconds or 0, false) end
function w3.weather.random(blendSeconds) return w3.call("RequestRandomWeatherChange", blendSeconds or 0, false) end
function w3.weather.names()
  local _, names = w3.call("GetWeatherNames", {})
  return names or {}
end
function w3.weather.rain() return w3.call("GetRainStrength") end
function w3.weather.snow() return w3.call("GetSnowStrength") end

w3.env = {}
function w3.env.all()
  local defs = select(1, w3.call("GetAllEnvironmentDefinitions", {}))
  return defs or {}
end
function w3.env.active()
  local defs = select(1, w3.call("GetActiveAreaEnvironmentDefinitions", {}))
  return defs or {}
end
-- an environment definition (a .env resource) on top of the area's: returns an id for w3.env.deactivate
function w3.env.activate(def, priority, blend, blendInSeconds)
  if type(def) == "string" then def = w3.load(def) or error("no environment " .. def) end
  return w3.call("ActivateEnvironmentDefinition", def, priority or 100, blend or 1, blendInSeconds or 0)
end
function w3.env.deactivate(id, blendOutSeconds) w3.call("DeactivateEnvironment", id, blendOutSeconds or 0) end
function w3.env.fake_time(hour)  -- the light of another hour (nil: back to the real one)
  if hour == nil then w3.call("DisableFakeEnvTime") else w3.call("ForceFakeEnvTime", hour) end
end

-- Camera ----------------------------------------------------------------------------------------------------
w3.camera = {}
local function director() return w3.global("theCamera") end
function w3.camera.pos() return director():GetCameraPosition() end
function w3.camera.rot() return director():GetCameraRotation() end
function w3.camera.forward() return director():GetCameraForward() end
function w3.camera.heading() return director():GetCameraHeading() end
-- The game camera pulls its field of view back to its own within about 3 s (measured); w3.camera.fov(d) holds
-- d by setting it every tick, w3.camera.fov(false) gives it back. The hold belongs to the mod that set it.
local fovHold
function w3.camera.fov(degrees)
  if degrees == false then
    if fovHold then w3.off_tick(fovHold) fovHold = nil end
  elseif degrees ~= nil then
    -- 0, 1e15 or NaN, set every tick, stop the game
    if type(degrees) ~= "number" or not (degrees >= 1 and degrees <= 179) then
      error("the field of view is 1 to 179 degrees (false gives it back)", 2)
    end
    if fovHold then w3.off_tick(fovHold) end
    game():GetGameCamera():SetFov(degrees)
    fovHold = w3.on_tick(function()
      local c = game():GetGameCamera()
      if c then c:SetFov(degrees) end
    end)
  end
  return director():GetFov()
end
function w3.camera.free(on)  -- the free camera; nil only reads
  if on ~= nil then game():EnableFreeCamera(on and true or false) end
  return game():IsFreeCameraEnabled()
end
function w3.camera.move(pos, rot)  -- the free camera to a place (rot: {Pitch, Yaw, Roll})
  game():MoveFreeCamera(pos, rot or {Pitch = 0, Yaw = 0, Roll = 0})
end
-- where a world position is on the screen: x, y from 0 (left, top) to 1, or nil when it is off the screen. The game's
-- ratio runs from -1 to 1 with y growing downwards (measured: Geralt's feet 0.80, his head -0.19).
function w3.camera.to_screen(pos)
  -- W = 1: a table of X, Y, Z alone has W = 0, and the game takes that as a direction (every point near the same
  -- pixel)
  pos = {X = pos.X or pos[1] or 0, Y = pos.Y or pos[2] or 0, Z = pos.Z or pos[3] or 0, W = 1}
  local ok, x, y = director():WorldVectorToViewRatio(pos, 0, 0)
  if not ok or x < -1 or x > 1 or y < -1 or y > 1 then return nil end
  return (x + 1) / 2, (y + 1) / 2
end
function w3.camera.shake(strength) w3.call("GCameraShake", strength or 0.5) end

-- Stats, abilities, buffs, level ------------------------------------------------------------------------------
w3.stats = {}
local function stat_name(s) return enum_name("EBaseCharacterStats", s) end
function w3.stats.get(stat, actor)
  actor = actor or player()
  local n = stat_name(stat)
  return actor:GetStat(n), actor:GetStatMax(n)
end
function w3.stats.set(stat, value, actor)
  actor = actor or player()
  actor:ForceSetStat(stat_name(stat), value)
  return actor:GetStat(stat_name(stat))
end
function w3.stats.health(actor)
  actor = actor or player()
  return actor:GetHealth(), actor:GetMaxHealth()
end
function w3.stats.abilities(actor)
  actor = actor or player()
  local list = select(1, actor:GetCharacterStats():GetAbilities({}, false))
  return list or {}
end
function w3.stats.has_ability(name, actor) return (actor or player()):HasAbility(name) end
function w3.stats.add_ability(name, actor) return (actor or player()):AddAbility(name, false) end
function w3.stats.remove_ability(name, actor) (actor or player()):RemoveAbility(name) end
local function effect_name(e) return enum_name("EEffectType", e) end
function w3.stats.buff(effect, actor)  -- a buff by its effect type ("EET_WellFed" or "wellfed")
  actor = actor or player()
  return actor:AddEffectDefault(effect_name(effect), actor, "tw3se")
end
function w3.stats.unbuff(effect, actor) (actor or player()):RemoveAllBuffsOfType(effect_name(effect)) end
function w3.stats.has_buff(effect, actor) return (actor or player()):HasBuff(effect_name(effect)) end
function w3.stats.buffs(actor)
  local out = {}
  for _, b in ipairs((actor or player()):GetBuffs() or {}) do out[#out + 1] = b:GetEffectType() end
  return out
end
function w3.stats.level(actor) return (actor or player()):GetLevel() end
function w3.stats.add_xp(points) player():AddPoints("EExperiencePoint", points, false) end

-- Inventory and money --------------------------------------------------------------------------------------
w3.inv = {}
local function inv(entity) return (entity or player()):GetInventory() end
function w3.inv.add(item, count, entity) return inv(entity):AddAnItem(item, count or 1) end
function w3.inv.remove(item, count, entity) return inv(entity):RemoveItemByName(item, count or 1) end
function w3.inv.count(item, entity) return inv(entity):GetItemQuantityByName(item) end
function w3.inv.items(entity)  -- {name, count, id} of each item
  local i = inv(entity)
  local ids = i:GetAllItems({})  -- void: the out array is the first value
  local out = {}
  for _, id in ipairs(ids or {}) do out[#out + 1] = {name = i:GetItemName(id), count = i:GetItemQuantity(id), id = id} end
  return out
end
function w3.inv.money(entity) return inv(entity):GetMoney() end
function w3.inv.add_money(amount, entity) inv(entity):AddMoney(amount) return inv(entity):GetMoney() end
function w3.inv.remove_money(amount, entity) inv(entity):RemoveMoney(amount) return inv(entity):GetMoney() end
function w3.inv.equip(id) return player():EquipItem(id) end
function w3.inv.unequip(id) return player():UnequipItem(id) end
function w3.inv.price(id, entity) return inv(entity):GetItemPrice(id) end

-- Horse and boat --------------------------------------------------------------------------------------------
w3.horse = {}
function w3.horse.get() return player():GetHorseWithInventory() end   -- Roach, while she is in the world
function w3.horse.mounted() return player():GetHorseCurrentlyMounted() end
function w3.horse.summon() game():OnSpawnPlayerHorse() end         -- as the whistle does
function w3.horse.manager() return player():GetHorseManager() end
function w3.horse.items()  -- what Roach carries (saddle, bags, blinders, trophy)
  local m = w3.horse.manager()
  local i = m and m:GetInventoryComponent()
  if not i then return {} end
  local ids = i:GetAllItems({})
  local out = {}
  for _, id in ipairs(ids or {}) do out[#out + 1] = {name = i:GetItemName(id), id = id} end
  return out
end
function w3.vehicle() return player():GetUsedVehicle() end         -- the horse or boat in use, or nil
)TW3SELIB" R"TW3SELIB(
-- Round 2 (a second literal: MSVC takes at most 16 KB in one) -----------------------------------------------------------
local function vec(p) return {X = p.X or p[1] or 0, Y = p.Y or p[2] or 0, Z = p.Z or p[3] or 0, W = 1} end
local function ahead(dist)  -- a point in front of the player, on the ground under it
  local p = player()
  local pos, h = p:GetWorldPosition(), math.rad(p:GetHeading())
  return {X = pos.X - math.sin(h) * dist, Y = pos.Y + math.cos(h) * dist, Z = pos.Z, W = 1}
end
w3.ahead = ahead

-- an entity from a template (a path or a CEntityTemplate) at a position (default 3 m in front of the player); it is
-- not saved with the game. Not the player's own template (a second player takes over Geralt's keys).
function w3.spawn(template, pos, yaw)
  if type(template) == "string" then template = w3.load(template) or error("no template " .. template) end
  pos = pos and vec(pos) or ahead(3)
  if yaw == nil then yaw = (player():GetHeading() + 180) % 360 end  -- facing the player
  return game():CreateEntity(template, pos, {Pitch = 0, Yaw = yaw, Roll = 0}, true, false, false, "PM_DontPersist", {})
end
function w3.destroy(entity) if entity and w3.valid(entity) then entity:Destroy() end end

-- AI ------------------------------------------------------------------------------------------------------------
w3.ai = {}
function w3.ai.attitude(npc, other) return npc:GetAttitude(other or player()) end
function w3.ai.set_attitude(npc, attitude, other) npc:SetAttitude(other or player(), enum_name("EAIAttitude", attitude)) end
function w3.ai.reset_attitude(npc, other) npc:ResetAttitude(other or player()) end
function w3.ai.group(npc) return npc:GetAttitudeGroup() end
function w3.ai.set_group(npc, group, priority)
  npc:SetTemporaryAttitudeGroup(group, enum_name("EAttitudeGroupPriority", priority or "AGP_Default"))
end
function w3.ai.reset_group(npc, priority)
  npc:ResetTemporaryAttitudeGroup(enum_name("EAttitudeGroupPriority", priority or "AGP_Default"))
end
-- a behaviour tree on top of the NPC's own (a class name such as "CAIMoveToAction" or an IAITree object); `setup`
-- fills its parameters before it starts: setup(tree). Returns an id for w3.ai.cancel.
function w3.ai.force(npc, tree, priority, setup)
  if type(tree) == "string" then
    local t = w3.new(tree, npc)
    t:OnCreated()
    tree = t
  end
  if setup then setup(tree) end
  return npc:ForceAIBehavior(tree, enum_name("EArbitratorPriorities", priority or "BTAP_Emergency"))
end
function w3.ai.cancel(npc, id) return npc:CancelAIBehavior(id) end
-- follows the player through the game's own tree CAIFollowAction (actionParams.ws), at BTAP_AboveIdle: a fight
-- (BTAP_Combat) goes first, then it follows again. opts: distance (2.5 m), run (catch up running), teleport (appear
-- near the player when catchup m behind, 40), target (a tag, "PLAYER"). Returns the id for w3.ai.cancel.
function w3.ai.follow(npc, opts)
  opts = opts or {}
  return w3.ai.force(npc, "CAIFollowAction", opts.priority or "BTAP_AboveIdle", function(t)
    local p = t.params
    p.targetTag = opts.target or "PLAYER"
    p.followDistance = opts.distance or 2.5
    p.moveType = opts.run == false and "MT_Walk" or "MT_Run"
    p.teleportToCatchup = opts.teleport ~= false
    p.cachupDistance = opts.catchup or 40
  end)
end
-- a companion: friendly to the player, follows, immortal when opts.immortal; the follow's id for w3.ai.cancel
function w3.ai.companion(npc, opts)
  opts = opts or {}
  w3.ai.set_attitude(npc, "AIA_Friendly")
  if opts.immortal then w3.ai.immortal(npc, true) end
  return w3.ai.follow(npc, opts)
end
-- walks (or runs) to a point through the game's own tree CAIMoveToPoint: an NPC refuses a bare ActionMoveTo outside
-- its AI (measured: false from ActionMoveToAsync, the forced tree walks it there). Needs an NPC with an AI of
-- its own (a crowd NPC spawned outside a community has none). Returns the id for w3.ai.cancel.
function w3.ai.move_to(npc, pos, run, priority)
  local p = vec(pos)
  return w3.ai.force(npc, "CAIMoveToPoint", priority or "BTAP_AboveCombat", function(t)
    local d = t.params.destinationPosition
    d.X, d.Y, d.Z, d.W = p.X, p.Y, p.Z, 1
    t.params.moveType = run and "MT_Run" or "MT_Walk"
  end)
end
function w3.ai.stop(npc) npc:ActionCancelAll() end
function w3.ai.var(npc, name, value)  -- a behaviour graph variable (a number)
  if value ~= nil then npc:SetBehaviorVariable(name, value) end
  return npc:GetBehaviorVariable(name)
end
function w3.ai.immortal(npc, on) npc:SetImmortalityMode(on and "AIM_Invulnerable" or "AIM_None", "AIC_Default") end
function w3.ai.kill(npc) npc:Kill("tw3se", true) end
function w3.ai.in_combat(npc) return npc:IsInCombat() end
function w3.ai.target(npc) return npc:GetTarget() end

-- Effects and light ---------------------------------------------------------------------------------------------
w3.fx = {}
function w3.fx.play(entity, effect, bone)
  if bone then return entity:PlayEffectOnBone(effect, bone) end
  return entity:PlayEffect(effect)
end
function w3.fx.stop(entity, effect) if effect then return entity:StopEffect(effect) end entity:StopAllEffects() end
function w3.fx.active(entity, effect) return entity:IsEffectActive(effect) end
function w3.fx.intensity(entity, effect, value) entity:SetEffectIntensity(effect, value) end

-- dynamic decals (decal.cpp): the game's way of painting blood, here with a picture of your own
w3.decal = {}
local function decal_rgba(c, def)  -- {r, g, b, a} / {Red = ..} / 0xRRGGBBAA -> the game's Color bytes, red first
  if c == nil then return def end
  local r, g, b, a
  if type(c) == "number" then
    r, g, b, a = (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255
  else
    r, g, b, a = c.Red or c.r or c[1] or 255, c.Green or c.g or c[2] or 255, c.Blue or c.b or c[3] or 255,
                 c.Alpha or c.a or c[4] or 255
  end
  return (r & 255) | ((g & 255) << 8) | ((b & 255) << 16) | ((a & 255) << 24)
end
local function decal_vec(v)
  if v == nil then return nil end
  return {v.X or v.x or v[1] or 0, v.Y or v.y or v[2] or 0, v.Z or v.z or v[3] or 0}
end
local function decal_up(dir, roll)  -- the picture's top for a turn of `roll` degrees around the direction
  local d = dir
  local l = math.sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3])
  if l < 1e-6 then return {0, 1, 0} end
  d = {d[1] / l, d[2] / l, d[3] / l}
  local u = math.abs(d[3]) > 0.9 and {0, 1, 0} or {0, 0, 1}
  local k = u[1] * d[1] + u[2] * d[2] + u[3] * d[3]
  u = {u[1] - k * d[1], u[2] - k * d[2], u[3] - k * d[3]}
  local ul = math.sqrt(u[1] * u[1] + u[2] * u[2] + u[3] * u[3])
  u = {u[1] / ul, u[2] / ul, u[3] / ul}
  if not roll or roll == 0 then return u end
  local a = math.rad(roll)
  local c, s = math.cos(a), math.sin(a)
  local x = {d[2] * u[3] - d[3] * u[2], d[3] * u[1] - d[1] * u[3], d[1] * u[2] - d[2] * u[1]}  -- dir x up
  return {u[1] * c + x[1] * s, u[2] * c + x[2] * s, u[3] * c + x[3] * s}
end
local function decal_row(t)
  local pos = decal_vec(t.pos) or error("a decal needs pos", 3)
  local dir = decal_vec(t.dir) or {0, 0, -1}
  local size = t.size
  local w, h, d
  if type(size) == "table" then
    w, h, d = size[1] or size.width, size[2] or size.height, size[3] or size.depth
  elseif type(size) == "number" then
    w, h, d = size, size, size
  end
  w, h = w or t.width or 0.2, h or t.height or w or 0.2
  d = d or t.depth or math.max(w, h)
  return {
    pos = pos, dir = dir, up = decal_vec(t.up) or decal_up(dir, t.roll),
    width = w, height = h, depth = d,
    diffuse = t.diffuse or error("a decal needs diffuse (a texture path)", 3), normal = t.normal,
    color = decal_rgba(t.color, nil), spec_color = decal_rgba(t.spec_color, nil),
    specularity = t.specularity, spec_scale = t.spec_scale, spec_base = t.spec_base,
    life = t.life or 0, fade_in = t.fade_in, fade_out = t.fade_out,
    projection = t.sphere and 1 or 0, double_sided = t.double_sided and 1 or 0, priority = t.priority and 1 or 0,
    static_only = t.static_only and 1 or 0, depth_fade = t.depth_fade, normal_fade_bias = t.normal_fade_bias,
    normal_fade_scale = t.normal_fade_scale, hide_distance = t.hide_distance, atlas = t.atlas, _b7c = t._b7c, _b7e = t._b7e,
  }
end
local function decal_on(on)
  if on == nil then return nil end
  if type(on) == "table" then return on end
  return {on}
end
function w3.decal.spawn(t)  -- -> id, or nil and the reason
  local ids, err = w3._decal_spawn({decal_row(t)}, decal_on(t.on))
  if ids[1] == 0 then return nil, err end
  return ids[1]
end
function w3.decal.spawn_many(list, on)  -- -> ids (0 for one that failed), the first reason
  local rows = {}
  for i, t in ipairs(list) do rows[i] = decal_row(t) end
  return w3._decal_spawn(rows, decal_on(on))
end
w3.decal.remove = w3._decal_remove
function w3.decal.clear(entity) return w3._decal_clear(entity) end
w3.decal.count = w3._decal_count
w3.decal.per_mesh = w3._decal_per_mesh  -- ([n]): how many decals one mesh takes (the engine: 2, up to 127)
function w3.decal.color(id, color, spec)  -- spec: {color, specularity, scale, base}
  spec = spec or {}
  return w3._decal_color(id, decal_rgba(color, 0xffffffff), decal_rgba(spec.color, 0xffffffff),
                         spec.specularity or -1, spec.scale or 0, spec.base or 1)
end
function w3.decal.limits(values)  -- the engine's decal settings ({["group/name"] = value}); with values: sets them
  local out = {}
  for _, v in ipairs(w3.config_list("decal")) do
    local group, name = v.path:match("^(.*)/([^/]+)$")
    if group then
      if values and values[name] ~= nil then w3.config(group, name, values[name]) end
      out[v.path] = w3.config(group, name)
    end
  end
  return out
end

w3.light = {}
local LIGHTS = {"CPointLightComponent", "CSpotLightComponent"}
function w3.light.of(entity)  -- the light components of an entity
  local out = {}
  for _, cls in ipairs(LIGHTS) do
    for _, c in ipairs(entity:GetComponentsByClassName(cls) or {}) do out[#out + 1] = c end
  end
  return out
end
function w3.light.near(radius, pos)  -- light components within radius of a point (default the player)
  pos = pos and vec(pos) or player():GetWorldPosition()
  local out = {}
  for _, cls in ipairs(LIGHTS) do
    for _, c in ipairs(w3.all(cls)) do
      local ok, d = pcall(function() return w3.call("VecDistance", pos, c:GetWorldPosition()) end)
      if ok and d <= radius then out[#out + 1] = c end
    end
  end
  return out
end
-- changes a light: {color = {r, g, b}, brightness = n, radius = n, on = bool}; color 0..255
function w3.light.set(light, opts)
  if opts.color then  -- field by field through the struct's view: the colour changes in place
    local c, col = opts.color, light.color
    col.Red, col.Green, col.Blue = c.r or c[1] or 255, c.g or c[2] or 255, c.b or c[3] or 255
    col.Alpha = c.a or c[4] or 255
  end
  if opts.brightness then light.brightness = opts.brightness end
  if opts.radius then light.radius = opts.radius end
  if opts.on ~= nil then light.isEnabled = opts.on and true or false end
  return light
end
-- a light of its own at a position: a hand torch whose meshes are hidden, its light changed by opts (w3.light.set)
function w3.light.spawn(pos, opts)
  local e = w3.spawn("environment\\decorations\\light_sources\\torch_hand\\torch_hand.w2ent", pos and vec(pos) or ahead(2), 0)
  if not e then return nil end
  for _, m in ipairs(e:GetComponentsByClassName("CMeshComponent") or {}) do pcall(function() m:SetVisible(false) end) end
  pcall(function() e:StopAllEffects() end)  -- the flame and the smoke
  for _, l in ipairs(w3.light.of(e)) do w3.light.set(l, opts or {}) end
  return e, w3.light.of(e)[1]
end

-- Game files changed while the game runs (the watch on tw3se\files does it by itself each second)
w3.files = {}
function w3.files.reload(path)  -- tw3se\files\<path> loads anew at its next use; true, or nil and the reason
  return w3._files_reload(path)
end
-- a game file's new contents for this session only, from memory (nothing written, gone at the next start): the next
-- load reads them; what is already in the world keeps the old version. true, or nil and the reason
w3.files.set = w3._files_set
-- the game's own contents again
w3.files.clear = w3._files_clear
-- A game file under a second path too (same bytes, same .buffer files). Loaded from that path it is a resource of
-- its own. The folder of the new path has to exist; true, or nil and the reason
w3.files.alias = w3._files_alias

-- The engine's configuration variables (1725: Rendering, PostProcess, the path tracer, shadows, LOD, physics,
-- gameplay ...): w3.config(group, name) reads one, w3.config(group, name, value) sets it from its next use on.
local function config_value(s)
  if s == "true" then return true elseif s == "false" then return false end
  return tonumber(s) or s
end
function w3.config(group, name, value)
  local v, err = w3._config(group, name, value)
  if v == nil then error(err, 2) end
  return config_value(v)
end
function w3.config_list(filter)  -- {{path = "Rendering/UseUmbraOcclusion", type = "bool"}, ...}
  local out = {}
  for _, line in ipairs(w3._config_list(filter)) do
    local path, t = line:match("^(.-)\t(.*)$")
    out[#out + 1] = {path = path, type = t}
  end
  return out
end

-- the mesh of a running mesh component (the component builds its render proxy anew)
w3.set_mesh = w3._set_mesh

-- A template's components on a running entity, the way the game adds an appearance's parts
-- (CAppearanceComponent.IncludeAppearanceTemplate); w3.detach takes them off again
local function appearance_of(entity)
  return entity:GetComponentByClassName("CAppearanceComponent") or error("the entity has no CAppearanceComponent", 3)
end
local function template_arg(t)
  if type(t) == "string" then return w3.load(t) or error("no template " .. t, 3) end
  return t
end
function w3.attach(entity, template)
  local t = template_arg(template)
  appearance_of(entity):IncludeAppearanceTemplate(t)
  return t
end
function w3.detach(entity, template)
  appearance_of(entity):ExcludeAppearanceTemplate(template_arg(template))
end

)TW3SELIB" R"TW3SELIB(
-- Material parameters. A mesh's materials belong to the mesh, so a change counts for every entity with that mesh
-- that appears from then on (a new spawn, an object streamed in); what is on screen keeps the values it was made with.
w3.material = {}
function w3.material.of(entity)  -- {component, index, material, class} for each material of the entity's meshes
  local out = {}
  for _, c in ipairs(entity:GetComponentsByClassName("CMeshComponent") or {}) do
    local m = c.mesh
    if m then
      for i = 1, #m.materials do
        local mi = m.materials[i]
        if mi then out[#out + 1] = {component = c, index = i, material = mi, class = w3.classof(mi)} end
      end
    end
  end
  return out
end
w3.material.params = w3._mat_params                -- {name = value} of a CMaterialInstance
function w3.material.get(mi, name) return w3._mat_params(mi)[name] end
w3.material.set = w3._mat_set                       -- (mi, name, value): Color as {Red = .., Green = .., Blue = ..}
function w3.material.apply(entity)  -- the entity's meshes drawn anew with the materials' current values
  local n = 0
  for _, c in ipairs(entity:GetComponentsByClassName("CMeshComponent") or {}) do
    if w3._refresh(c) then n = n + 1 end
  end
  return n
end

-- the render proxy of a drawable component made anew from its current data; false when it is not in the world
w3.refresh = w3._refresh

-- Everything that can be copied from one object or struct into another of the same class, field by field, into
-- arrays and nested structs. Returns the paths it skipped (pointers to objects the source owns, engine-only types).
local function is_array(v)
  local mt = type(v) == "userdata" and getmetatable(v)
  return type(mt) == "table" and mt.__name == "tw3se.array"
end
local function is_struct(v) return type(v) == "userdata" and tostring(v):find("(struct)", 1, true) ~= nil end
local function copy_into(dst, src, path, skipped)
  for name in pairs(w3.fields(src)) do
    local p = path .. name
    local s = src[name]
    if not pcall(function() dst[name] = s end) then
      if is_array(s) and pcall(function() dst[name] = {} end) then
        local d = dst[name]
        for i = 1, #s do
          local e = s[i]
          if is_struct(e) then
            if pcall(function() d:insert({}) end) then copy_into(d[#d], e, p .. "[" .. i .. "].", skipped)
            else skipped[#skipped + 1] = p .. "[" .. i .. "]" break end
          elseif not pcall(function() d:insert(e) end) then
            skipped[#skipped + 1] = p
            break
          end
        end
      elseif is_struct(s) then
        copy_into(dst[name], s, p .. ".", skipped)
      else
        skipped[#skipped + 1] = p
      end
    end
  end
  return skipped
end
function w3.copy(dst, src) return copy_into(dst, src, "", {}) end

-- Blend shapes: deltas from a TWDF file move a mesh's vertices on the GPU, weighted, before the game draws; every
-- shader of the game draws the moved vertices. The render mesh is shared by every entity with that mesh: a set
-- changes all of them.
w3.deform = {}
local function render_mesh(c)  -- a CMeshComponent or a CMesh -> the address of its render mesh
  local m = w3.classof(c) == "CMesh" and c or c.mesh
  local rm = m and w3.peek(w3.addr(m), 0xa0)
  if not rm or rm < 0x10000 then error("the mesh is not loaded (no render mesh)", 3) end
  return rm
end
function w3.deform.meshes(entity)  -- {name = component} of the entity's CMeshComponents
  local out = {}
  for _, c in ipairs(entity:GetComponentsByClassName("CMeshComponent") or {}) do out[tostring(c.name)] = c end
  return out
end
-- A component's own copy goes with its figure (Destroy) or when it gets another mesh, and the game frees the copy's
-- render mesh soon after. Its sets end at the next tick, without a write, before the memory is handed on. The set
-- knows its render mesh by address only: after the free it read the old values from the freed memory, took the mesh
-- for alive and wrote the mesh's box into it at each weight change (a crash minutes later, with figures that hold
-- copies with weights set every tick). w3.deform._watch = false turns the check off (for a test).
local owned = {}  -- address of the component -> its shared mesh
local watched, watchTick = {}, nil  -- render mesh address of an own copy with sets -> {component, its address}
w3.deform._watch = true
local function watch_copy(c, rm)
  watched[rm] = {c, w3.addr(c)}
  if watchTick then return end
  watchTick = w3._as("tw3se", w3.on_tick, function()
    if not w3.deform._watch then return end
    for addr, w in pairs(watched) do
      local ok, now, alive = false, nil, w3.valid(w[1])
      if alive then ok, now = pcall(render_mesh, w[1]) end
      if not ok or now ~= addr then
        watched[addr] = nil
        if not alive then owned[w[2]] = nil end  -- a new component at that address gets a copy of its own
        local n = w3._bs_forget(addr)
        if n > 0 then print(string.format("w3.deform: %d set(s) ended with a copy whose figure or mesh went", n)) end
      end
    end
    if next(watched) == nil then
      w3.off_tick(watchTick)
      watchTick = nil
    end
  end)
end
function w3.deform.load(c, path, range)  -- a set from tw3se\<path>; range = the largest |weight| planned (1)
  local rm = render_mesh(c)
  local set, err = w3._bs_load(rm, path, range)
  if set and w3.classof(c) ~= "CMesh" and owned[w3.addr(c)] then watch_copy(c, rm) end
  return set, err
end
w3.deform.set = w3._bs_set                -- (set, shape name or index, weight)
function w3.deform.weights(set, t)        -- {shape = weight, ...} or {w1, w2, ...}
  for k, v in pairs(t) do w3._bs_set(set, k, v) end
end
w3.deform.names = w3._bs_names            -- the set's shape names, in file order
w3.deform.state = w3._bs_state            -- "loading", "ready", "lost" (mesh gone or loaded anew), "none"
w3.deform.unload = w3._bs_unload          -- the original back
w3.deform.normals = w3._bs_normals        -- (set or live mesh, true): normals follow the moved triangles
function w3.deform.dump(c, path, now)     -- the mesh's base positions and normals per chunk to tw3se\<path> (TWDB)
  return w3._bs_dump(render_mesh(c), path, now)  -- now: as moved, with a set loaded
end
-- A mesh of its own for one component, so a set changes this entity only: the mesh file under a second path (a new
-- one per call), loaded as a mesh of its own and set into the component. Its render mesh is there a few ticks later
-- (w3.deform.loaded). release() gives the component the shared mesh back.
function w3.deform.own(c)
  local m = c.mesh or error("the component has no mesh", 2)
  local key = w3.addr(c)
  if owned[key] then return m end
  local src = m:GetPath()
  local stem = src:match("^(.*)%.w2mesh$") or error("not a .w2mesh: " .. src, 2)
  local n, path, ok, err = 0
  repeat
    repeat
      n = n + 1
      path = stem .. "__own" .. n .. ".w2mesh"
    until not w3._diskfile(path)
    ok, err = w3.files.alias(src, path)
  until ok or not err:find("hash", 1, true) or n > 1000  -- (a name whose hash is taken: the next number)
  if not ok then error(err, 2) end
  local copy = w3.load(path) or error("the copy did not load: " .. path, 2)
  owned[key] = m
  w3.set_mesh(c, copy)
  return copy
end
function w3.deform.release(c)  -- the shared mesh back; false if the component had none of its own
  local key = w3.addr(c)
  local m = owned[key]
  if not m then return false end
  owned[key] = nil
  local ok, rm = pcall(render_mesh, c)  -- the copy's sets end here, without a write into the copy going away
  if ok then w3._bs_forget(rm) end
  w3.set_mesh(c, m)
  return true
end
function w3.deform.loaded(c) return (pcall(render_mesh, c)) end
-- fn(count) after TW3SE ended every set because the game rebuilds its render resources (a graphics option applied
-- through the options, path tracing among them): load the sets again (w3.deform.state gives "none" for the old ones)
w3.deform._reset_fns = {}
function w3.deform.on_reset(fn) table.insert(w3.deform._reset_fns, fn) end  -- the component's mesh has its render mesh
-- Live meshes: the component gets a copy of its mesh (w3.deform.own) whose positions and normals are written each
-- frame (w3.deform.live_write, or a plugin's LiveMeshWrite). The id at once, "loading" until the copy is loaded and
-- drawn; live_close gives the shared mesh back.
local live = {}  -- id -> component
function w3.deform.live(c)
  w3.deform.own(c)
  local id = w3._lm_new()
  live[id] = c
  local ticks, tick = 0, nil
  tick = w3._as("tw3se", w3.on_tick, function()
    ticks = ticks + 1
    local done, why = false, nil
    if not live[id] then
      done = true
    elseif not w3.valid(c) or ticks > 1800 then
      why = w3.valid(c) and "the copy did not load in 1800 ticks" or "the component went before its copy loaded"
    else
      local ok, rm = pcall(render_mesh, c)
      if ok then  -- (the render mesh comes a few ticks before its vertex buffer)
        local attached, err = w3._lm_attach(id, rm)
        if attached then
          watch_copy(c, rm)
          done = true
        elseif not err:find("vertex buffer was not found", 1, true) then
          why = err
        elseif ticks > 1800 then
          why = "the copy did not load in 1800 ticks: " .. err
        end
      end
    end
    if why then
      print("w3.deform.live " .. id .. ": " .. why)
      w3._lm_fail(id)
      done = true
    end
    if done then w3.off_tick(tick) end
  end)
  return id
end
function w3.deform.live_close(id)  -- the shared mesh back; false if there was no such live mesh
  local c = live[id]
  live[id] = nil
  local had = w3._lm_close(id)
  if c and w3.valid(c) then w3.deform.release(c) end
  return had or c ~= nil
end
w3.deform.live_write = w3._lm_write    -- (id, chunk from 0, {x, y, z, ...}, {nx, ny, nz, ...} or nil)
w3.deform.live_info = w3._lm_info      -- (id, chunk) -> vertex count, chunk count
w3.deform.live_bounds = w3._lm_bounds  -- (id, {min x, y, z}, {max x, y, z}): the box positions are stored in
-- The skinning shader hook (skin_hook = on): a mesh gets a slot, the patched skinning shaders push the skinned
-- vertices of its draws out of capsules (up to 8 a slot, in the mesh's model space, metres).
w3.skinhook = {}
function w3.skinhook.add(c) return w3._skin_add(render_mesh(c)) end  -- slot 1..8 | nil, error
w3.skinhook.remove = w3._skin_remove
-- (slot, {{a = {x, y, z}, b = {x, y, z}, radius = r, strength = s, falloff = k, inward = true}, ...}), up to 32:
-- strength 1 pushes to the surface; falloff (metres) makes the push soft, from k beyond the surface on; inward draws
-- the skin within k outside the surface onto it
function w3.skinhook.capsules(slot, list)
  local flat = {}
  for _, c in ipairs(list or {}) do
    local a, b = c.a or c[1], c.b or c[2]
    for _, v in ipairs({a[1] or a.X or a.x, a[2] or a.Y or a.y, a[3] or a.Z or a.z, c.radius or 0.1,
                        b[1] or b.X or b.x, b[2] or b.Y or b.y, b[3] or b.Z or b.z, c.strength or 1,
                        c.falloff or 0, c.inward and -1 or 1, 0, 0}) do
      flat[#flat + 1] = v
    end
  end
  return w3._skin_caps_ex(slot, flat)
end
w3.skinhook.stats = w3._skin_stats
-- (slot, path): the skinned positions the ray tracing skinning computes for the slot's mesh, a few frames later
-- in tw3se\<path> (needs ray tracing on); one float4 a vertex at (chunk's stream 0 - base) / 16 + its number
w3.skinhook.capture = w3._skin_capture
-- w3.skinhook.overlay(slot, size, colour, material): the skin overlay of a slot, two strings of 4 * size * size
-- bytes RGBA8 at the mesh's UV. colour: red first, alpha = how much covers the skin. material: R wet, G matte,
-- B height. nil: that plane clear. size 0: off.
function w3.skinhook.overlay(slot, size, colour, material)
  local ok, err = w3._skin_overlay_raw(slot, 0, size, colour)
  if ok and size > 0 then ok, err = w3._skin_overlay_raw(slot, 1, size, material) end
  return ok, err
end
w3.skinhook.mush = w3._skin_mush            -- (slot, iterations): Delta Mush on the slot's mesh (0 = off)
w3.skinhook.mush_mask = w3._skin_mush_mask  -- (slot, chunk, {m, ...}): how much of it each vertex takes (0..1)
w3.skinhook.state = w3._skin_state      -- (slot) -> "none", "active", "lost"
-- (slot, chunk, {w, ...}): one weight a vertex of the chunk (0 no push .. 1 all); {} takes them away
w3.skinhook.weights = w3._skin_weights
w3.skinhook.chunks = w3._skin_chunks    -- (slot) -> {vertex count of chunk 0, ...}: chunk i is [i + 1]

)TW3SELIB" R"TW3SELIB(
-- J4: network. Requests and sockets work on threads of their own; a tick hands what arrived to the callbacks. A
-- callback that raises an error is logged and the others go on.
local net_handlers = {}  -- id -> function(event)
local net_seen = -1
local function net_watch()
  if w3.time() - net_seen < 1 then return end  -- the poller ran within the last second
  net_seen = w3.time()
  w3.on_tick(function()
    net_seen = w3.time()
    for _, e in ipairs(w3._net_poll()) do
      local h = net_handlers[e.id]
      if h then
        if e.kind == "http" or (e.kind == "close") or (e.kind == "connect" and e.error) then net_handlers[e.id] = nil end
        local ok, err = pcall(h, e)
        if not ok then print("network callback: " .. tostring(err)) end
      end
    end
  end)
end
w3.http = {}
-- opts: {method = "GET", url, headers = {name = value}, body, timeout = seconds}; fn(status, body, headers, err),
-- status 0 and err set when no answer came
function w3.http.request(opts, fn)
  local lines = {}
  for k, v in pairs(opts.headers or {}) do lines[#lines + 1] = k .. ": " .. v end
  local id = w3._net_http(opts.method or "GET", opts.url or error("a url is needed", 2), table.concat(lines, "\r\n"),
                          opts.body, math.floor((opts.timeout or 30) * 1000))
  net_handlers[id] = function(e) if fn then fn(e.status, e.body, e.headers, e.error) end end
  net_watch()
  return id
end
function w3.http.get(url, fn, opts)
  opts = opts or {}
  opts.url, opts.method = url, "GET"
  return w3.http.request(opts, fn)
end
function w3.http.post(url, body, fn, opts)
  opts = opts or {}
  opts.url, opts.method, opts.body = url, "POST", body
  return w3.http.request(opts, fn)
end
w3.net = {}
-- a TCP connection: handlers {on_connect(conn), on_data(conn, data), on_close(conn, err)}; conn:send(data), conn:close()
function w3.net.connect(host, port, handlers)
  handlers = handlers or {}
  local id = w3._net_connect(host, port)
  local conn = {id = id}
  function conn:send(data) return w3._net_send(id, data) end
  function conn:close() w3._net_close(id) end
  net_handlers[id] = function(e)
    if e.kind == "connect" then
      if e.error then if handlers.on_close then handlers.on_close(conn, e.error) end
      elseif handlers.on_connect then handlers.on_connect(conn) end
    elseif e.kind == "data" then if handlers.on_data then handlers.on_data(conn, e.body) end
    elseif e.kind == "close" then if handlers.on_close then handlers.on_close(conn, e.error) end end
  end
  net_watch()
  return conn
end
-- a UDP socket on this PC (127.0.0.1, port 0: any free one): fn(data, host, port) for each message;
-- sock:send(host, port, data), sock:close()
function w3.net.udp(port, fn)
  local id, err = w3._net_udp(port or 0)
  if not id then error(err, 2) end
  local sock = {id = id}
  function sock:send(host, p, data) return w3._net_send(id, data, host, p) end
  function sock:close() w3._net_close(id) end
  net_handlers[id] = function(e) if e.kind == "udp" and fn then fn(e.body, e.host, e.port) end end
  net_watch()
  return sock
end

-- Physics through the game's own functions ----------------------------------------------------------------------
w3.physics = {}
function w3.physics.bodies(entity)  -- the entity's components with a dynamic physics body (crates, barrels, debris)
  local out = {}
  local cs = entity.components   -- (GetComponentsByClassName gives the exact class only)
  for i = 1, cs and #cs or 0 do
    local c = cs[i]
    local ok, yes = pcall(function() return c:HasDynamicPhysic() end)
    if ok and yes then out[#out + 1] = c end
  end
  return out
end
local function body(entity) return w3.physics.bodies(entity)[1] or error("no dynamic physics body") end
function w3.physics.impulse(entity, v) body(entity):ApplyLocalImpulseToPhysicalObject(vec(v)) end
function w3.physics.torque(entity, v) body(entity):ApplyTorqueImpulseToPhysicalObject(vec(v)) end
function w3.physics.velocity(entity, v)  -- the body's velocity; with v: sets it
  local b = body(entity)
  if v then b:SetPhysicalObjectLinearVelocity(vec(v)) end
  return b:GetPhysicalObjectLinearVelocity()
end
function w3.physics.mass(entity) return body(entity):GetPhysicalObjectMass() end
function w3.physics.ragdoll(actor, on)  -- the game's ragdoll effect (as quests switch it), off again with false
  if on == false then actor:RemoveAllBuffsOfType("EET_Ragdoll") else actor:AddEffectDefault("EET_Ragdoll", nil) end
end
function w3.physics.ragdolled(entity) return entity:IsRagdolled() end
function w3.physics.collisions(actor, on) actor:EnableCollisions(on ~= false) end
function w3.physics.kinematic(entity, on) entity:SetKinematic(on ~= false) end
-- the PhysX scene of the world (its address); the gravity of everything PhysX simulates in it (crates, ragdolls,
-- debris; figures walk with their own). {X, Y, Z} in m/s^2, the game's is {0, 0, -9.81}
local function px_scene()
  local world = game():GetWorld() or error("no world (main menu)", 3)
  return w3._px_scene(w3.addr(world)) or error("no PhysX scene in the world", 3)
end
w3.physics.scene = px_scene
function w3.physics.gravity(v)
  local x, y, z
  if v then x, y, z = w3._px_gravity(px_scene(), v.X or v[1] or 0, v.Y or v[2] or 0, v.Z or v[3] or 0)
  else x, y, z = w3._px_gravity(px_scene()) end
  return {X = x, Y = y, Z = z}
end

-- The game's sounds (Wwise events, switches, states, parameters), beside w3.sound's own files ---------------------
w3.sound = w3.sound or {}
local function snd() return w3.global("theSound") end
function w3.sound.event(name, entity, bone)  -- at an entity (its position, its bone), or without: the game's 2D sound
  if entity then entity:SoundEvent(name, bone) else snd():SoundEvent(name) end
end
function w3.sound.switch(group, state, entity)
  if entity then entity:SoundSwitch(group, state) else snd():SoundSwitch(group, state) end
end
function w3.sound.state(group, state) snd():SoundState(group, state) end   -- music and mood states
-- Wwise banks: the game's, or your own .bnk from tw3se\files (packed into soundspc.cache at the start); its events
-- then play with w3.sound.event
function w3.sound.load_bank(name, async) snd():SoundLoadBank(name, async or false) end
function w3.sound.unload_bank(name) snd():SoundUnloadBank(name) end
function w3.sound.bank_loaded(name) return snd():SoundIsBankLoaded(name) end
function w3.sound.param(name, value, entity, duration)
  if entity then entity:SoundParameter(name, value, nil, duration or 0) else snd():SoundParameter(name, value, duration or 0) end
end

-- Animation ---------------------------------------------------------------------------------------------------
w3.anim = {}
function w3.anim.play(actor, name, opts)  -- a slot animation; true when it started
  opts = opts or {}
  return actor:ActionPlaySlotAnimationAsync(opts.slot or "NPC_ANIM_SLOT", name, opts.blend_in or 0.2,
    opts.blend_out or 0.2, opts.loop or false)
end
function w3.anim.var(entity, name, value)  -- a behaviour graph variable; with value: sets it
  if value ~= nil then entity:SetBehaviorVariable(name, value) end
  return entity:GetBehaviorVariable(name)
end
function w3.anim.event(entity, name, force)  -- a behaviour graph event
  if force then return entity:RaiseForceEvent(name) end
  return entity:RaiseEvent(name)
end
function w3.anim.mimic(actor, name, blend) return actor:PlayMimicAnimationAsync(name, blend or 0.2) end
)TW3SELIB" R"TW3SELIB(
-- One more animation set for a figure that is in the world: its animations play by name from then on, and
-- they win over the figure's own of the same name. The set stays loaded while it is in (kept here). A path or a
-- loaded CSkeletalAnimationSet; an actor (its root animated component) or a CAnimatedComponent.
local added_sets = {}  -- "<component address> <set address>" -> the set
local function anim_component(target)
  if w3.classof(target) and w3.class(w3.classof(target)) and target.GetRootAnimatedComponent then
    local ok, c = pcall(function() return target:GetRootAnimatedComponent() end)
    if ok and c then return c end
  end
  return target
end
function w3.anim.add_set(target, set)
  if type(set) == "string" then set = w3.load(set) or error("no animation set " .. set, 2) end
  local comp = anim_component(target)
  w3._anim_add_set(comp, set)
  added_sets[w3.addr(comp) .. " " .. w3.addr(set)] = set
  return set
end
-- The bones of a dangle rig that the dangle neither maps from the figure nor simulates get their rest pose each frame
-- (such bones keep memory garbage, and a mesh skinned to them flies apart). For every figure that uses the rig;
-- the rig stays loaded while it is named here. w3.anim.rest_bones(path, false) takes it off.
-- w3.anim.rest_bones(path, {names}): the same for bones a mod adds to a figure's main rig (no animation of the game
-- has a track for them, and a standing figure holds them in a pose of their own); each named bone gets its rest
-- pose on its parent every frame, before the figure's meshes are skinned and before plugins see the pose.
-- A rig named by its path is looked up again after every load and every few seconds: a world that ends unloads it,
-- and the figures of the next one use a new object (a rig named in the main menu is filled after the save
-- loaded).
local rest_rigs = {}  -- key -> {rig, addr, path, bones}
local function rest_follow()
  for _, r in pairs(rest_rigs) do
    if r.path then
      local now = w3.load(r.path)
      local addr = now and w3.addr(now)
      if addr and addr ~= r.addr then
        w3._rest_bones(r.addr, false)
        local ok = pcall(w3._rest_bones, now, r.bones or true)
        if ok then r.rig, r.addr = now, addr end
      end
    end
  end
end
local rest_watch
function w3.anim.rest_bones(rig, on)
  local path = type(rig) == "string" and rig or nil
  if path then rig = w3.load(path) or error("no rig " .. tostring(path), 2) end
  local key = path or w3.addr(rig)
  local old = rest_rigs[key]
  if on == false then
    if old then w3._rest_bones(old.addr, false) end
    rest_rigs[key] = nil
    return true
  end
  local bones = type(on) == "table" and on or nil
  if old and (old.addr ~= w3.addr(rig) or (old.bones == nil) ~= (bones == nil)) then
    w3._rest_bones(old.addr, false)
  end
  w3._rest_bones(rig, bones or true)
  rest_rigs[key] = {rig = rig, addr = w3.addr(rig), path = path, bones = bones}
  if path and not rest_watch then
    rest_watch = true
    w3._as("tw3se", w3.on_load, rest_follow)
    w3._as("tw3se", w3.every, 5, rest_follow)
  end
  return true
end
function w3.anim.remove_set(target, set)
  local comp = anim_component(target)
  w3._anim_remove_set(comp, set)
  added_sets[w3.addr(comp) .. " " .. w3.addr(set)] = nil
  return true
end
-- A copy of a template whose entities spawn with more animation sets (.w2anims, the game's or your own from
-- tw3se\files): one more CAnimAnimsetsParam in its parameters, for the component the template's own animsets name.
-- Spawn from the copy, then w3.anim.play the new animations.
local sets_sources = {}  -- address -> template: the copies point at its parameters, so it stays loaded
function w3.anim.with_sets(template, sets)
  if type(template) == "string" then template = w3.load(template) or error("no template " .. template, 2) end
  sets_sources[w3.addr(template)] = template
  local t = w3.new("CEntityTemplate")
  w3.copy(t, template)
  local params, component = {}, ""
  for i = 1, #t.templateParams do
    local p = t.templateParams[i]
    params[#params + 1] = p
    if w3.classof(p) == "CAnimAnimsetsParam" and component == "" then component = p.componentName end
  end
  local list = {}
  for _, s in ipairs(sets) do
    list[#list + 1] = type(s) == "string" and (w3.load(s) or error("no animation set " .. s, 2)) or s
  end
  local prm = w3.new("CAnimAnimsetsParam")
  prm.name = "tw3se"
  prm.componentName = component
  prm.animationSets = list
  params[#params + 1] = prm
  t.templateParams = params
  return t
end

-- Figures ----------------------------------------------------------------------------------------------------
w3.figure = {}
function w3.figure.appearance(entity, name)  -- the appearance's name; with name: changes it
  if name then
    if entity.SetAppearance then entity:SetAppearance(name) else entity:ApplyAppearance(tostring(name)) end
  end
  return tostring(entity:GetAppearance())
end
function w3.figure.appearances(entity)  -- the names its template offers
  local t = entity.template
  local out = {}
  if t then for _, a in ipairs(t.appearances:to_table()) do out[#out + 1] = tostring(a.name) end end
  return out
end
function w3.figure.line(actor, id, subtitle) actor:PlayLine(id, subtitle ~= false) end   -- a voiced line by string id
function w3.figure.voiceset(actor, name) return actor:PlayVoiceset(100, name, true) end
-- Geralt's head, as the barber changes it (beard and face; CHeadManagerComponent)
local function head_manager(actor)
  return (actor or player()):GetComponentByClassName("CHeadManagerComponent") or error("no head manager (Geralt has one)", 3)
end
function w3.figure.head(name, actor)  -- the head's name; with name (or a number): that head
  local h = head_manager(actor)
  if type(name) == "number" then h:SetCustomHeadNum(name) elseif name then h:SetCustomHead(name) end
  return h:GetCurHeadName()
end
function w3.figure.head_reset(actor) head_manager(actor):RemoveCustomHead() end
-- HairWorks hair and fur. The settings belong to the fur resource, so a change counts for every entity with it;
-- w3.figure.fur_color redraws the component it is given.
function w3.figure.furs(entity)  -- the entity's fur components (HairWorks has to be on in the game's settings)
  local out = {}
  local cs = entity.components
  for i = 1, cs and #cs or 0 do
    local c = cs[i]
    if c and w3.classof(c) == "CFurComponent" then out[#out + 1] = c end
  end
  return out
end
-- A colour texture wins over the colour (the wolves have one), so fur_color takes the texture of the part it sets.
-- The first change of a resource is kept; fur_reset puts colours and textures back.
local fur_kept = {}
function w3.figure.fur_color(c, root, tip)  -- Color tables {Red, Green, Blue, Alpha}; either may be nil
  local col = c.furMesh.graphicalMaterials.color
  local key = w3.addr(c.furMesh)
  fur_kept[key] = fur_kept[key] or {root = col.rootColor, tip = col.tipColor, root_tex = col.rootColorTex,
                                    tip_tex = col.tipColorTex}
  if root then col.rootColor, col.rootColorTex = root, nil end
  if tip then col.tipColor, col.tipColorTex = tip, nil end
  w3.refresh(c)
  return col.rootColor, col.tipColor
end
function w3.figure.fur_reset(c)  -- the resource's colours and textures as before the first fur_color
  local key = w3.addr(c.furMesh)
  local k = fur_kept[key]
  if not k then return false end
  local col = c.furMesh.graphicalMaterials.color
  col.rootColor, col.tipColor, col.rootColorTex, col.tipColorTex = k.root, k.tip, k.root_tex, k.tip_tex
  fur_kept[key] = nil
  w3.refresh(c)
  return true
end

-- Drawing in the world (inside w3.on_draw) ----------------------------------------------------------------------
w3.draw = w3.draw or {}
function w3.draw.line(x1, y1, x2, y2, colour, width)  -- built from small squares (the overlay draws rectangles)
  width = width or 2
  local dx, dy = x2 - x1, y2 - y1
  local steps = math.max(1, math.floor(math.max(math.abs(dx), math.abs(dy)) / width))
  if steps > 2000 then steps = 2000 end
  for i = 0, steps do
    local t = i / steps
    w3.draw.rect(x1 + dx * t - width / 2, y1 + dy * t - width / 2, width, width, colour)
  end
end
local function screen(pos)
  local x, y = w3.camera.to_screen(vec(pos))
  if not x then return nil end
  local w, h = w3.screen_size()
  return x * w, y * h
end
w3.draw.to_pixels = screen
function w3.draw.text_at(pos, text, colour)  -- text centred over a world position; false when it is off screen
  local x, y = screen(pos)
  if not x then return false end
  local w = w3.draw.size and w3.draw.size(text) or 0
  w3.draw.text(x - (w > 0 and w / 2 or #text * 4), y, colour or 0xFFFFFFFF, text)
  return true
end
function w3.draw.marker_at(pos, size, colour)
  local x, y = screen(pos)
  if not x then return false end
  size = size or 8
  w3.draw.rect(x - size / 2, y - size / 2, size, size, colour or 0xFFFF00FF)
  return true
end
function w3.draw.line_at(a, b, colour, width)
  local x1, y1 = screen(a)
  local x2, y2 = screen(b)
  if not x1 or not x2 then return false end
  w3.draw.line(x1, y1, x2, y2, colour or 0xFFFFFFFF, width)
  return true
end
function w3.draw.label(entity, text, colour, height)  -- text over an entity's head
  local p = entity:GetWorldPosition()
  return w3.draw.text_at({X = p.X, Y = p.Y, Z = p.Z + (height or 2.1)}, text, colour)
end
)TW3SELIB" R"TW3SELIB(
-- Round 3: the game's UI, Gwent, quests --------------------------------------------------------------------------------
w3.hud = {}
local function hud() return game():GetHud() or error("no HUD (main menu)") end
function w3.hud.module(name) return hud():GetHudModule(name) end          -- "BuffsModule", "MinimapModule" ...
function w3.hud.modules()
  local out = {}
  for _, n in ipairs(hud().hudModulesNames) do out[#out + 1] = n end
  return out
end
function w3.hud.enabled(name, on)  -- a HUD module on or off; nil only reads
  local m = w3.hud.module(name) or error("no HUD module " .. tostring(name))
  if on ~= nil then m:SetEnabled(on and true or false) end
  return m:GetEnabled()
end
function w3.hud.visible(on)  -- the whole HUD, as the player's own HUD toggle
  if on ~= nil then hud():ForceShow(on and true or false, "HVS_User") end
  return hud().m_visibleHudByUser
end
-- the game's ShowNotification takes milliseconds (its own call: 9000)
function w3.hud.message(text, seconds) game():GetGuiManager():ShowNotification(text, (seconds or 3) * 1000) end
function w3.hud.oneliner(text, entity) hud():ShowOneliner(text, entity or player()) end   -- a line over a figure
function w3.hud.hide_oneliner(entity) hud():HideOneliner(entity or player()) end
-- w3.hud.ask(text, fn [, {title, buttons = "yes_no" | "ok_cancel" | "ok"}]): the game's own message window
-- (CR4GuiManager.ShowUserDialogAdv); fn(yes, action) when the player answers: true for Yes and OK. The answer comes
-- through CR4GuiManager.UserDialogCallback with the message's id (TW3SE's from 7300001 on), which a hook takes.
do
  local asks, nextAsk, askHooked = {}, 7300000, false
  local BUTTONS = {yes_no = "UDB_YesNo", ok_cancel = "UDB_OkCancel", ok = "UDB_Ok"}
  function w3.hud.ask(text, fn, opts)
    opts = opts or {}
    local kind = BUTTONS[opts.buttons or "yes_no"] or error("w3.hud.ask: buttons is yes_no, ok_cancel or ok", 2)
    if fn ~= nil and type(fn) ~= "function" then error("w3.hud.ask(text, fn): fn is a function", 2) end
    if not askHooked then
      askHooked = true
      w3._as("tw3se", w3.hook, "CR4GuiManager", "UserDialogCallback", function(self, args, orig)
        local a = asks[args[1]]
        local r = orig()
        if a then
          asks[args[1]] = nil
          if a.fn then w3._call_as(a.owner, a.fn, args[2] == "UMA_Ok" or args[2] == "UMA_Yes", args[2]) end
        end
        return r
      end)
    end
    nextAsk = nextAsk + 1
    asks[nextAsk] = {fn = fn, owner = w3._mod()}
    game():GetGuiManager():ShowUserDialogAdv(nextAsk, w3.tr(opts.title or ""), w3.tr(text), false, kind)
    return nextAsk
  end
end

w3.gwent = {}
local function gwent() return game():GetGwintManager() end
function w3.gwent.collection()  -- {card, count} of each card the player owns
  local g, out = gwent(), {}
  for _, c in ipairs(g:GetPlayerCollection() or {}) do out[#out + 1] = {card = c, count = g:GetPlayerCardCount(c)} end
  return out
end
function w3.gwent.count(card) return gwent():GetPlayerCardCount(card) end
function w3.gwent.has(card) return gwent():HasCardInCollection(card) end
-- n copies, 1 to 1000: one engine call each (n = 1e15 would hold the game for good)
local function copies(n)
  n = n or 1
  if math.type(n) ~= "integer" and not (type(n) == "number" and n == math.floor(n)) or n < 1 or n > 1000 then
    error("the number of copies is 1 to 1000", 3)
  end
  return math.tointeger(n)
end
function w3.gwent.add(card, n) for _ = 1, copies(n) do gwent():AddCardToCollection(card) end return w3.gwent.count(card) end
function w3.gwent.remove(card, n)
  for _ = 1, copies(n) do gwent():RemoveCardFromCollection(card) end
  return w3.gwent.count(card)
end
function w3.gwent.cards()  -- every card of the game: {name, title, power, faction, index}
  local g, out = gwent(), {}
  for _, d in ipairs(g:GetCardDefs() or {}) do
    out[#out + 1] = {name = d.cardName, title = d.title, power = d.power, faction = d.faction,
                     index = g:GetGwentCardIndex(d.cardName)}
  end
  return out
end
function w3.gwent.leaders() return gwent():GetPlayerLeaderCollection() or {} end
function w3.gwent.deck(faction)  -- the selected deck's faction; with a faction: select it
  if faction then gwent():SetSelectedPlayerDeck(enum_name("eGwintFaction", faction)) end
  return gwent():GetSelectedPlayerDeck()
end
function w3.gwent.unlocked(faction) return gwent():IsDeckUnlocked(enum_name("eGwintFaction", faction)) end
function w3.gwent.unlock(faction) gwent():UnlockDeck(enum_name("eGwintFaction", faction)) end

w3.quest = {}
local function journal() return game():GetJournalManager() end
local function loc(id) return id and id ~= 0 and w3.call("GetLocStringById", id) or "" end
function w3.quest.list(status)  -- the quests in the journal: {title, status, entry}; status "JS_Active" ... filters
  local j = journal()
  local entries = j:GetActivatedOfType("CJournalQuest", {}) or {}
  local out = {}
  for _, e in ipairs(entries) do
    local st = j:GetEntryStatus(e)
    if not status or st == status then out[#out + 1] = {title = loc(e:GetTitleStringId()), status = st, entry = e} end
  end
  return out
end
function w3.quest.tracked()  -- the tracked quest: its title and entry, or nil
  local q = journal():GetTrackedQuest()
  if not q then return nil end
  return loc(q:GetTitleStringId()), q
end
function w3.quest.track(entry) journal():SetTrackedQuest(entry) end
function w3.quest.status(entry) return journal():GetEntryStatus(entry) end
-- the player's own map pins: {x, y, type, area, id}
function w3.quest.pins()
  local m, out = game():GetCommonMapManager(), {}
  for i = 0, m:GetUserMapPinCount() - 1 do
    local ok, id, area, x, y, kind = m:GetUserMapPinByIndex(i, 0, "", 0, 0, 0)
    if ok then out[#out + 1] = {id = id, area = area, x = x, y = y, type = kind} end
  end
  return out
end
-- puts a pin of the player's own at a position (default: the player's), or takes the one there away again
-- (the area as the map menu names it: from the world's path). Kind 0 is the waypoint (one at a time, a new one
-- replaces it), kinds 1 to 7 the menu's pin symbols. Returns ok, the id added, the id removed.
function w3.quest.toggle_pin(pos, kind)
  kind = kind or 1
  local m = game():GetCommonMapManager()
  -- despite its name the call sets the limits (at most 1 waypoint and 30 other pins), as the map menu does on every
  -- open; a limit of 0 left by another call refuses every pin
  m:GetUserMapPinLimits(1, 30)
  local p = pos and vec(pos) or player():GetWorldPosition()
  local area = m:GetAreaFromWorldPath(game():GetWorld():GetDepotPath(), true)
  local ok, added, removed = m:ToggleUserMapPin(area, {X = p.X, Y = p.Y, Z = 0, W = 1}, kind, false, true, 0, 0)
  return ok ~= 0 and ok ~= false, added, removed
end

w3.fact = {}
function w3.fact.get(name) return w3.call("FactsQuerySum", name) end
function w3.fact.add(name, value, seconds) w3.call("FactsAdd", name, value or 1, seconds or -1) return w3.fact.get(name) end
function w3.fact.set(name, value)  -- exactly this value from now on
  w3.call("FactsRemove", name)
  if value and value ~= 0 then w3.call("FactsAdd", name, value, -1) end
  return w3.fact.get(name)
end
function w3.fact.remove(name) return w3.call("FactsRemove", name) end
function w3.fact.exists(name) return w3.call("FactsDoesExist", name) end

-- Round 4: the world and scenes ---------------------------------------------------------------------------------------
w3.world = {}
local function world() return game():GetWorld() end
function w3.world.path() return world():GetDepotPath() end
-- the layer group an entity is in ("settlements\\kaer_morhen\\..." under the world's folder), or nil
function w3.world.group_of(entity)
  local layer = entity:GetParent()
  local ok, path = pcall(function() return layer:GetPath() end)
  if not ok or type(path) ~= "string" then return nil end
  local dir = w3.world.path():gsub("[^\\]+$", "")          -- levels\kaer_morhen\
  local rel = path:sub(1, #dir):lower() == dir:lower() and path:sub(#dir + 1) or path
  if not rel:find("\\", 1, true) then return "" end        -- a layer right in the world's folder: the root
  return (rel:gsub("\\[^\\]+$", ""))                       -- the folders, without the layer file
end
function w3.world.layers(filter)  -- the paths of the loaded layers (optionally those containing filter)
  local out = {}
  for _, l in ipairs(w3.all("CLayer")) do
    local ok, p = pcall(function() return l:GetPath() end)
    if ok and type(p) == "string" and p ~= "" and (not filter or p:find(filter, 1, true)) then out[#out + 1] = p end
  end
  table.sort(out)
  return out
end
-- Buildings, walls and rocks are no entities: cooking merges a layer's static meshes with their collisions and decals
-- into its sector data (CLayer.sectorData). An object whose position lies far away when its layer attaches never
-- streams in (measured), so hiding one = moving it away and loading its layer group again.
local SECTOR_TYPES = {"mesh", "collision", "decal"}
local function group_of_path(path)
  local dir = w3.world.path():gsub("[^\\]+$", "")
  local rel = path:sub(1, #dir):lower() == dir:lower() and path:sub(#dir + 1) or path
  if not rel:find("\\", 1, true) then return "" end
  return (rel:gsub("\\[^\\]+$", ""))
end
-- A path finds its layer in a table made from one pass over w3.all("CLayer"), made again when a path is missing or its
-- layer is gone. One pass takes 0.22 s (15402 layers in Velen); once per call, a loop over w3.world.layers()
-- would stand the game still for minutes.
local layer_by_path = {}
local function layer_obj(layer)
  if type(layer) ~= "string" then return layer end
  local key = layer:lower()
  local l = layer_by_path[key]
  if l and w3.valid(l) then return l end
  layer_by_path = {}
  for _, c in ipairs(w3.all("CLayer")) do
    local ok, p = pcall(function() return c:GetPath() end)
    if ok and p then layer_by_path[p:lower()] = c end
  end
  l = layer_by_path[key]
  if l then return l end
  error("no loaded layer " .. layer, 3)
end
local function sector_of(layer)
  local l = layer_obj(layer)
  local sd = l.sectorData
  if not sd then error("the layer has no merged geometry", 3) end
  return w3.addr(sd), l:GetPath()
end
function w3.world.sector(layer)  -- {index, type, radius, pos} of each merged object of a layer (path or CLayer)
  local out = {}
  for i, o in ipairs(w3._sector((sector_of(layer))) or {}) do
    out[i] = {index = i, type = SECTOR_TYPES[o.type] or o.type, radius = o.radius, pos = {X = o.X, Y = o.Y, Z = o.Z}}
  end
  return out
end
function w3.world.sector_near(pos, radius)  -- the merged objects of the loaded layers within radius of pos
  local p, out = vec(pos), {}
  for _, path in ipairs(w3.world.layers()) do
    local ok, objs = pcall(w3.world.sector, path)
    for _, o in ipairs(ok and objs or {}) do
      local d = math.sqrt((o.pos.X - p.X) ^ 2 + (o.pos.Y - p.Y) ^ 2 + (o.pos.Z - p.Z) ^ 2)
      if d <= radius then o.layer, o.distance = path, d out[#out + 1] = o end
    end
  end
  table.sort(out, function(a, b) return a.distance < b.distance end)
  return out
end
local sector_moved = {}  -- layer path (lower case) -> {index -> original position}
local function reload_group(path)
  local g = group_of_path(path)
  w3.run(function()
    w3.world.hide_group(g)
    w3.wait_frames(10)
    w3.world.show_group(g)
  end)
end
function w3.world.sector_hide(layer, indices, now)  -- objects away (a mesh takes its collision along); now: reload
  local addr, path = sector_of(layer)
  local objs = w3._sector(addr)
  local key = path:lower()
  sector_moved[key] = sector_moved[key] or {}
  local saved = sector_moved[key]
  for _, i in ipairs(type(indices) == "table" and indices or {indices}) do
    local list = {i}
    if objs[i] and objs[i].type == 1 and objs[i + 1] and objs[i + 1].type == 2 then list[2] = i + 1 end
    for _, k in ipairs(list) do
      local o = objs[k] or error("no object " .. k .. " in " .. path, 2)
      if not saved[k] then saved[k] = {o.X, o.Y, o.Z} end
      w3._sector_pos(addr, k, 1e7, 1e7, -1e5)
    end
  end
  if now then reload_group(path) end
end
function w3.world.sector_restore(layer, now)  -- every object sector_hide moved, back in place
  local addr, path = sector_of(layer)
  local saved = sector_moved[path:lower()] or {}
  for k, p in pairs(saved) do w3._sector_pos(addr, k, p[1], p[2], p[3]) end
  sector_moved[path:lower()] = nil
  if now then reload_group(path) end
end
-- A colour layer over the whole terrain (CRenderCommand_SetTerrainCustomOverlay), not public: the terrain vanishes
-- while it is set, and a second call crashes the game. Kept for the analysis.
function w3.world._terrain_overlay(width, height, pixels)
  local clip = world().terrainClipMap or error("the world has no terrain", 2)
  if width == nil then return w3._terrain_overlay(w3.addr(clip)) end
  if type(pixels) == "function" then
    local rows = {}
    for y = 0, height - 1 do
      local row = {}
      for x = 0, width - 1 do
        local r, g, b, a = pixels(x, y)
        row[#row + 1] = string.char(r or 0, g or 0, b or 0, a or 255)
      end
      rows[#rows + 1] = table.concat(row)
    end
    pixels = table.concat(rows)
  end
  return w3._terrain_overlay(w3.addr(clip), width, height, pixels)
end
)TW3SELIB" R"TW3SELIB(
-- Raises (metres > 0) or lowers the terrain within radius metres of a point, smoothly to the edge: the
-- picture when the camera brings that part of the terrain into view again. The collision only with
-- terrain_collision = on in tw3se.config (the game ended itself 25 s after a heightfield swap). It lasts until the
-- tile builds its terrain again (the player leaves the area and comes back). Returns the height map texels and the
-- collision pieces it changed.
function w3.world.terrain_raise(pos, radius, metres)
  local clip = world().terrainClipMap or error("the world has no terrain", 2)
  pos = vec(pos)
  return w3._terrain_raise(w3.addr(clip), pos.X, pos.Y, radius, metres)
end
function w3.world.terrain_height(pos)  -- the terrain's own height at a point (its height map), or nil
  local clip = world().terrainClipMap
  if not clip then return nil end
  pos = vec(pos)
  local ok, h = pcall(w3._terrain_probe, w3.addr(clip), pos.X, pos.Y, 0)
  if ok then return h end
end
function w3.world.show_group(group) world():ShowLayerGroup(group) end
function w3.world.hide_group(group) world():HideLayerGroup(group) end
function w3.world.ground(pos)  -- the height of the ground (physics) under a point, or nil
  local ok, z = world():PhysicsCorrectZ(vec(pos), 0)
  if ok then return z end
end
function w3.world.nav_z(pos, below, above)  -- the navmesh height near a point, or nil
  local p = vec(pos)
  local ok, z = world():NavigationComputeZ(p, p.Z - (below or 5), p.Z + (above or 5), 0)
  if ok then return z end
end
function w3.world.water(pos) return world():GetWaterLevel(vec(pos)), world():GetWaterDepth(vec(pos)) end
function w3.world.trace(a, b, groups)  -- hit, position, normal of a ray through the static world
  return world():StaticTrace(vec(a), vec(b), {X = 0, Y = 0, Z = 0, W = 1}, {X = 0, Y = 0, Z = 0, W = 1},
                             groups or {"Static", "Terrain"})
end
function w3.world.sphere(pos, radius)  -- the entities whose collision overlaps a sphere
  local _, ents = world():SphereOverlapTest({}, vec(pos), radius)
  return ents or {}
end
function w3.world.safe_spot(pos, space, radius)  -- a free navmesh spot near a point, or nil
  local ok, spot = world():NavigationFindSafeSpot(vec(pos), space or 0.5, radius or 5, {X = 0, Y = 0, Z = 0, W = 1})
  if ok then return spot end
end
function w3.world.terrain()  -- the terrain's size in metres and its tile count
  local ok, size, tiles = world():GetTerrainParameters(0, 0)
  if ok then return size, tiles end
end

-- --- the world's shape for placing things: rays with every hit, the floors of a spot, whether a person stands or
-- walks there, whether a box fits. Rays are the trace manager's RayCastSync (every hit along the ray, the component of
-- an entity's), the volumes SweepTest (a sphere drawn along a segment: a capsule). Houses, walls and rocks are merged
-- geometry without a component; terrain is told by its height map.
local SOLID = {"Static", "Terrain", "Destructible", "Door", "RigidBody", "Platforms", "Fence", "Foliage", "Boat"}
local Z0 = {X = 0, Y = 0, Z = 0, W = 1}
local function groups_of(opts)
  if opts and opts.groups then return opts.groups end
  local g = {table.unpack(SOLID)}
  if opts and opts.people then g[#g + 1], g[#g + 2] = "Character", "Ragdoll" end
  return g
end
local function v3(v) return {X = v.X, Y = v.Y, Z = v.Z} end
local function owner(h, c)
  if c then
    h.what, h.component, h.class = "entity", c, w3.classof(c)
    local ok, e = pcall(function() return c:GetEntity() end)
    if ok then h.entity = e end
    return
  end
  local t = w3.world.terrain_height(h.pos)
  h.what = (t and math.abs(t - h.pos.Z) < 0.15) and "terrain" or "world"
end
function w3.world.cast(a, b, opts)  -- every hit of a ray, nearest first: {pos, normal, dist (m), what, entity, component}
  a, b = vec(a), vec(b)
  local len = math.sqrt((b.X - a.X) ^ 2 + (b.Y - a.Y) ^ 2 + (b.Z - a.Z) ^ 2)
  local _, res = world():GetTraceManager():RayCastSync(a, b, {}, groups_of(opts))
  local out = {}
  for _, r in ipairs(res or {}) do
    local h = {pos = v3(r.position), normal = v3(r.normal), dist = r.distance * len}
    if not (opts and opts.bare) then owner(h, r.component) end
    out[#out + 1] = h
  end
  table.sort(out, function(x, y) return x.dist < y.dist end)
  return out
end
local function ceiling(x, y, z, opts)  -- the height of the first surface above a point, or nil
  local hits = w3.world.cast({X = x, Y = y, Z = z}, {X = x, Y = y, Z = z + 40}, opts)
  return hits[1] and hits[1].pos.Z or nil
end
-- the upward surfaces at a point, top first: {z, slope, headroom, covered, what, entity}. covered: another surface lies
-- less than 0.3 m above it (a plank over the ground, a floor's lower layer), nothing to stand on
function w3.world.floors(pos, opts)
  opts = opts or {}
  local p = vec(pos)
  local hits = w3.world.cast({X = p.X, Y = p.Y, Z = p.Z + (opts.above or 30)}, {X = p.X, Y = p.Y, Z = p.Z - (opts.below or 30)}, opts)
  local out = {}
  for _, h in ipairs(hits) do
    local last = out[#out]
    if h.normal.Z > 0.05 and (not last or last.z - h.pos.Z > 0.03) then
      local top = opts.headroom ~= false and ceiling(p.X, p.Y, h.pos.Z + 0.05, opts) or nil
      out[#out + 1] = {z = h.pos.Z, slope = math.deg(math.acos(math.min(1, h.normal.Z))), headroom = top and top - h.pos.Z,
                       covered = last ~= nil and last.z - h.pos.Z < 0.3, what = h.what, entity = h.entity,
                       component = h.component}
    end
  end
  return out
end
-- the floor at x, y nearest to height z within reach (a covered one only when there is no other), or nil
local function floor_near(x, y, z, reach, opts, headroom)
  local best, under
  for _, f in ipairs(w3.world.floors({X = x, Y = y, Z = z}, {above = reach, below = reach, groups = opts and opts.groups,
                                                             people = opts and opts.people, headroom = headroom == true,
                                                             bare = headroom ~= true})) do
    if f.covered then
      if not under or math.abs(f.z - z) < math.abs(under.z - z) then under = f end
    elseif not best or math.abs(f.z - z) < math.abs(best.z - z) then
      best = f
    end
  end
  return best or under
end
local function swept(a, b, r, opts)  -- whether a sphere of radius r drawn from a to b touches anything: hit, where
  local hit, at = world():SweepTest(vec(a), vec(b), r, Z0, Z0, groups_of(opts))
  return hit, hit and at and v3(at) or nil
end
-- whether a person can stand at a point: {ok, ground, slope, headroom, water, why}. opts: radius (0.35), height
-- (1.8), step (0.5: how far the ground may lie from pos.Z), slope (45 degrees), people (true: other people count)
function w3.world.stand(pos, opts)
  opts = opts or {}
  local p = vec(pos)
  local r, height, step = opts.radius or 0.35, opts.height or 1.8, opts.step or 0.5
  local f = floor_near(p.X, p.Y, p.Z, step + 0.05, opts, true)
  local res = {ok = false}
  if not f or math.abs(f.z - p.Z) > step then
    res.why = "no ground within " .. step .. " m"
    return res
  end
  res.ground, res.slope, res.headroom = f.z, f.slope, f.headroom
  local level, depth = world():GetWaterLevel({X = p.X, Y = p.Y, Z = f.z, W = 1}), nil
  if level and level > f.z + 0.6 and level < f.z + 10 then
    res.water = level - f.z
    res.why = string.format("water %.1f m deep", res.water)
    return res
  end
  if f.slope > (opts.slope or 45) then
    res.why = string.format("the ground is too steep (%.0f degrees)", f.slope)
    return res
  end
  if f.headroom and f.headroom < height then
    res.why = string.format("the ceiling is %.2f m above the ground", f.headroom)
    return res
  end
  local hit, at = swept({X = p.X, Y = p.Y, Z = f.z + r + 0.15}, {X = p.X, Y = p.Y, Z = f.z + height - r}, r, opts)
  if hit then
    res.why = string.format("something is in the way %.2f m above the ground", at and at.Z - f.z or 0)
    res.blocked = at
    return res
  end
  res.ok = true
  return res
end
)TW3SELIB" R"TW3SELIB(
-- whether a person walks from a to b in a straight line: {ok, reached, length, why}. Steps of opts.stride (0.25 m);
-- the ground may rise opts.climb (0.45 m) or fall opts.drop (1.0 m) from one step to the next.
function w3.world.walk(a, b, opts)
  opts = opts or {}
  a, b = vec(a), vec(b)
  local stride, climb, drop = opts.stride or 0.25, opts.climb or 0.45, opts.drop or 1.0
  local first = w3.world.stand(a, opts)
  if not first.ok then return {ok = false, reached = a, length = 0, why = "the start: " .. first.why} end
  local dx, dy = b.X - a.X, b.Y - a.Y
  local len = math.sqrt(dx * dx + dy * dy)
  local n = math.max(1, math.ceil(len / stride))
  local z, last = first.ground, {X = a.X, Y = a.Y, Z = first.ground}
  for i = 1, n do
    local x, y = a.X + dx * i / n, a.Y + dy * i / n
    local f = floor_near(x, y, z, math.max(climb, drop) + 0.05, opts)
    if not f or f.z > z + climb or f.z < z - drop then
      return {ok = false, reached = last, length = len * (i - 1) / n,
              why = f and string.format("a step of %.2f m", f.z - z) or "no ground"}
    end
    local s = w3.world.stand({X = x, Y = y, Z = f.z}, opts)
    if not s.ok then return {ok = false, reached = last, length = len * (i - 1) / n, why = s.why} end
    z, last = f.z, {X = x, Y = y, Z = f.z}
  end
  return {ok = true, reached = last, length = len}
end
-- box = {pos = the middle of its underside, size = {x, y, z} in its own axes, yaw}: whether it fits there:
-- {ok, rests (its corners and middle have ground under them within opts.rest, 0.1 m), lowest, highest, cuts, why}.
-- The volume is tested with spheres drawn upward on a grid over its underside (lifted opts.lift, 0.05 m, off the
-- ground): a thing thinner than the grid's gaps (a third of the smaller side, at most 0.25 m) can go unseen.
function w3.world.fits(box, opts)
  opts = opts or {}
  local p, s, yaw = vec(box.pos), box.size, math.rad(box.yaw or 0)
  local sx, sy, sz = s.x or s.X or s[1], s.y or s.Y or s[2], s.z or s.Z or s[3]
  local cx, cy = math.cos(yaw), math.sin(yaw)
  local function at(u, v) return p.X + u * cx - v * cy, p.Y + u * cy + v * cx end  -- u right, v forward
  local res = {ok = false}
  local hx, hy = sx / 2, sy / 2
  local lo, hi = 1e9, -1e9
  local spots = {{0, 0}, {-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}}
  for _, uv in ipairs(spots) do
    local x, y = at(uv[1], uv[2])
    local f = floor_near(x, y, p.Z, 1.0, opts)
    if not f then
      res.why = "no ground under a corner"
      return res
    end
    lo, hi = math.min(lo, f.z), math.max(hi, f.z)
  end
  res.lowest, res.highest = lo, hi
  res.rests = hi - lo <= (opts.rest or 0.1)
  local r = math.min(0.25, math.min(sx, sy) / 3)
  local lift = opts.lift or 0.05
  local nu, nv = math.max(1, math.ceil(sx / (r * 1.5))), math.max(1, math.ceil(sy / (r * 1.5)))
  res.cuts = {}
  for i = 0, nu do
    for j = 0, nv do
      local u = -hx + r + (sx - 2 * r) * i / nu
      local v = -hy + r + (sy - 2 * r) * j / nv
      if sx < 2 * r then u = 0 end
      if sy < 2 * r then v = 0 end
      local x, y = at(u, v)
      local bottom, top = hi + lift + r, p.Z + sz - r
      if top < bottom then top = bottom end
      local hit, where = swept({X = x, Y = y, Z = bottom}, {X = x, Y = y, Z = top}, r, opts)
      if hit then res.cuts[#res.cuts + 1] = where or {X = x, Y = y, Z = bottom} end
    end
  end
  if #res.cuts > 0 then
    res.why = #res.cuts .. " of its parts cut into the world"
  elseif not res.rests then
    res.why = string.format("the ground under it differs by %.2f m", hi - lo)
  else
    res.ok = true
  end
  return res
end
-- the nearest place within opts.radius (5 m) of box.pos where w3.world.fits says yes: the box there, and fits' answer
function w3.world.free_spot(box, opts)
  opts = opts or {}
  local p = vec(box.pos)
  local step = opts.step or 0.5
  for ring = 0, math.floor((opts.radius or 5) / step) do
    local n = ring == 0 and 1 or math.floor(2 * math.pi * ring)
    for k = 0, n - 1 do
      local a = 2 * math.pi * k / n
      local x, y = p.X + ring * step * math.cos(a), p.Y + ring * step * math.sin(a)
      local f = floor_near(x, y, p.Z, 1.0, opts)
      if f then
        local b = {pos = {X = x, Y = y, Z = f.z}, size = box.size, yaw = box.yaw}
        local r = w3.world.fits(b, opts)
        if r.ok then return b, r end
      end
    end
  end
  return nil
end
)TW3SELIB" R"TW3SELIB(
-- FNV-1a 64 over a depot path, as 16 hex digits: the hash the merged meshes name their mesh by
function w3.world.path_hash(path)
  local h = -3750763034362895579  -- 0xcbf29ce484222325
  for i = 1, #path do
    h = (h ~ path:lower():byte(i)) * 1099511628211
  end
  return string.format("%016x", h)
end
-- merged meshes (houses, walls, rocks) near a point, nearest first: {source ("world" or a layer's path), index, group
-- (the world's block: the layer it came from), hash, mesh (its depot path; the first call reads the bundles' tables),
-- kind ("mesh", "proxy": a house as it is drawn from afar, "volume": weather or sound), origin, axes, min, max (its box
-- in the world), size (in its own axes), yaw, distance (from the point to the box, 0 inside it), collision}. The world's own block holds the merged meshes of every layer of the world; a layer's block
-- (a quest's, a DLC's) holds its own.
local merged_layers = {at = -1e9, list = {}}  -- the layers with a block of their own: again every 2 s and in a new world
local function merged_blocks()
  local now, wa = os.clock(), w3.addr(world())
  if now - merged_layers.at > 2 or merged_layers.world ~= wa then
    local list = {}
    for _, c in ipairs(w3.all("CLayer")) do
      local ok, path = pcall(function() return c:GetPath() end)
      local sd = ok and c.sectorData
      if sd then list[#list + 1] = {addr = w3.addr(sd), source = path} end
    end
    merged_layers = {at = now, list = list, world = wa}
  end
  local blocks = {}
  local wb = w3._sector_world(world())
  if wb then blocks[1] = {addr = wb, source = "world"} end
  for _, b in ipairs(merged_layers.list) do blocks[#blocks + 1] = b end
  return blocks
end
-- the engine's angles of three axes (rows, in the world; a scaled axis is longer): {Pitch, Yaw, Roll} and the scale.
-- The axes are Roll x Pitch x Yaw (row vectors; measured 06.11.): Pitch about x, Yaw about z, Roll about y.
function w3.world.angles(axes)
  local function v(a) return {a.X or a[1], a.Y or a[2], a.Z or a[3]} end
  local x, y, z = v(axes[1]), v(axes[2]), v(axes[3])
  local function len(a) return math.sqrt(a[1] ^ 2 + a[2] ^ 2 + a[3] ^ 2) end
  local sx, sy, sz = len(x), len(y), len(z)
  local a13, a21, a22, a23, a33 = x[3] / sx, y[1] / sy, y[2] / sy, y[3] / sy, z[3] / sz
  return {Pitch = math.deg(math.asin(math.max(-1, math.min(1, a23)))), Yaw = math.deg(math.atan(-a21, a22)),
          Roll = math.deg(math.atan(-a13, a33))}, {x = sx, y = sy, z = sz}
end
local function merged_entry(m, source, p)
  local M, b = m.m, m.box
  local function len(i) return math.sqrt(M[i] ^ 2 + M[i + 1] ^ 2 + M[i + 2] ^ 2) end
  local lo, hi = m.min, m.max
  local rot, scale = w3.world.angles({{M[1], M[2], M[3]}, {M[4], M[5], M[6]}, {M[7], M[8], M[9]}})
  local dx = math.max(lo[1] - p.X, 0, p.X - hi[1])
  local dy = math.max(lo[2] - p.Y, 0, p.Y - hi[2])
  local dz = math.max(lo[3] - p.Z, 0, p.Z - hi[3])
  local mesh = w3._mesh_path(m.hash)
  local low = mesh and mesh:lower() or ""
  return {source = source, index = m.index, group = m.group, hash = m.hash, mesh = mesh, collision = m.collision,
          kind = low:find("proxy", 1, true) and "proxy" or low:find("volume", 1, true) and "volume" or "mesh",
          origin = {X = M[10], Y = M[11], Z = M[12]},
          axes = {{X = M[1], Y = M[2], Z = M[3]}, {X = M[4], Y = M[5], Z = M[6]}, {X = M[7], Y = M[8], Z = M[9]}},
          min = {X = lo[1], Y = lo[2], Z = lo[3]}, max = {X = hi[1], Y = hi[2], Z = hi[3]},
          size = {x = (b[4] - b[1]) * len(1), y = (b[5] - b[2]) * len(4), z = (b[6] - b[3]) * len(7)},
          rot = rot, scale = scale, yaw = rot.Yaw % 360, distance = math.sqrt(dx * dx + dy * dy + dz * dz)}
end
function w3.world.merged(pos, radius)
  local p, out = vec(pos), {}
  radius = radius or 20
  for _, b in ipairs(merged_blocks()) do
    for _, m in ipairs(w3._sector_meshes(b.addr, p.X, p.Y, p.Z, radius) or {}) do
      out[#out + 1] = merged_entry(m, b.source, p)
    end
  end
  table.sort(out, function(a, b) return a.distance < b.distance end)
  return out
end
)TW3SELIB" R"TW3SELIB(
-- Any mesh of the game in the world, with its collision, without a template file: an entity's compiled data
-- (CR2W) made here, a CGameplayEntity with one CStaticMeshComponent, spawned from a template in memory. Static
-- collision stays where it was made, so a move makes the entity anew.
local crc_table
local function crc32(s)
  if not crc_table then
    crc_table = {}
    for i = 0, 255 do
      local c = i
      for _ = 1, 8 do c = (c & 1) ~= 0 and (0xEDB88320 ~ (c >> 1)) or (c >> 1) end
      crc_table[i] = c
    end
  end
  local c = 0xFFFFFFFF
  for i = 1, #s do c = crc_table[(c ~ s:byte(i)) & 0xFF] ~ (c >> 8) end
  return c ~ 0xFFFFFFFF
end
local function name_hash(n)  -- a CR2W name's hash: FNV-1a 32 over its bytes and the closing 0
  if n == "" then return 0 end
  local h = 0x811C9DC5
  for i = 1, #n + 1 do h = ((h ~ (n:byte(i) or 0)) * 0x01000193) & 0xFFFFFFFF end
  return h
end
local function mesh_entity_data(mesh, scale)
  local names = {"", "CGameplayEntity", "isEnabled", "Bool", "CStaticMeshComponent", "name", "String", "mesh",
                 "handle:CMesh", "CMesh", "transform", "EngineTransform"}
  local ni = {}
  for i, n in ipairs(names) do ni[n] = i - 1 end
  local function prop(n, t, v) return string.pack("<I2I2I4", ni[n], ni[t], 4 + #v) .. v end
  local entity = "\0" .. prop("isEnabled", "Bool", "\1") .. "\0\0" .. string.rep("\0", 8) .. "\1" .. string.pack("<I4", 2) .. "\0\0"
  local comp = "\0" .. prop("name", "String", string.char(0x80 | 4) .. "mesh") .. prop("mesh", "handle:CMesh", string.pack("<i4", -1))
  if scale then comp = comp .. prop("transform", "EngineTransform", string.pack("<Bfff", 4, scale.x, scale.y, scale.z)) end
  comp = comp .. "\0\0"
  local strings, soff = {}, {}
  local slen = 0
  local function put(s)
    if not soff[s] then
      soff[s] = slen
      strings[#strings + 1] = s .. "\0"
      slen = slen + #s + 1
    end
    return soff[s]
  end
  for _, n in ipairs(names) do put(n) end
  put(mesh)
  local stab = table.concat(strings)
  local ntab = {}
  for _, n in ipairs(names) do ntab[#ntab + 1] = string.pack("<I4I4", soff[n], name_hash(n)) end
  ntab = table.concat(ntab)
  local itab = string.pack("<I4I2I2", soff[mesh], ni["CMesh"], 0)
  local ptab = string.rep("\0", 16)
  local pos = 0xA0
  local s_off = pos; pos = pos + #stab
  local n_off = pos; pos = pos + #ntab
  local i_off = pos; pos = pos + #itab
  local p_off = pos; pos = pos + #ptab
  local e_off = pos; pos = pos + 48
  local d_off = pos
  local etab = string.pack("<I2I2I4I4I4I4I4", ni["CGameplayEntity"], 0, 0, #entity, d_off, 0, crc32(entity)) ..
               string.pack("<I2I2I4I4I4I4I4", ni["CStaticMeshComponent"], 0, 1, #comp, d_off + #entity, 0, crc32(comp))
  local size = d_off + #entity + #comp
  local tables = string.pack("<I4I4I4", s_off, #stab, crc32(stab)) .. string.pack("<I4I4I4", n_off, #names, crc32(ntab)) ..
                 string.pack("<I4I4I4", i_off, 1, crc32(itab)) .. string.pack("<I4I4I4", p_off, 1, crc32(ptab)) ..
                 string.pack("<I4I4I4", e_off, 2, crc32(etab)) .. string.rep("\0", 12 * 5)
  local head = string.pack("<c4I4I4I8I4I4I4I4I4", "CR2W", 164, 0, 0, 0, size, size, 0xDEADBEEF, 6) .. tables
  head = head:sub(1, 32) .. string.pack("<I4", crc32(head)) .. head:sub(37)
  return head .. stab .. ntab .. itab .. ptab .. etab .. entity .. comp
end
local placed = {}  -- entity address -> {mesh, scale, template}
local function angles_arg(rot)
  if type(rot) == "number" then return {Pitch = 0, Yaw = rot, Roll = 0} end
  rot = rot or {}
  return {Pitch = rot.Pitch or 0, Yaw = rot.Yaw or 0, Roll = rot.Roll or 0}
end
-- a mesh (its depot path) at pos, turned by rot (a yaw in degrees or {Pitch, Yaw, Roll}); opts.scale = {x, y, z}.
-- -> the entity (a CGameplayEntity; not saved with the game)
function w3.world.place(mesh, pos, rot, opts)
  local s = opts and opts.scale
  if s and math.abs(s.x - 1) < 1e-4 and math.abs(s.y - 1) < 1e-4 and math.abs(s.z - 1) < 1e-4 then s = nil end
  if not w3.load(mesh) then error("no mesh " .. tostring(mesh), 2) end
  local data = mesh_entity_data(mesh, s)
  local t = w3.new("CEntityTemplate")
  t.flatCompiledData = {data:byte(1, -1)}
  local e = game():CreateEntity(t, vec(pos), angles_arg(rot), true, false, false, "PM_DontPersist", {})
  if not e then error("the game made no entity of " .. mesh, 2) end
  placed[w3.addr(e)] = {mesh = mesh, scale = s, template = t}
  return e
end
-- a placed mesh at another place (a new entity, the old one goes: static collision does not move) -> the new entity
function w3.world.move(entity, pos, rot)
  local p = placed[w3.addr(entity)] or error("not a mesh of w3.world.place", 2)
  local e = w3.world.place(p.mesh, pos, rot, {scale = p.scale})
  w3.world.remove(entity)
  return e
end
function w3.world.remove(entity)
  if entity and w3.valid(entity) then placed[w3.addr(entity)] = nil end
  w3.destroy(entity)
end
-- heights on a grid around a point: {step, cols, rows, x0, y0, z = {[row] = {[col] = height | false}}}. Each cell has
-- the floor nearest to pos.Z (opts.top: the highest surface instead, a roof over a room).
function w3.world.heightmap(pos, width, depth, step, opts)
  opts = opts or {}
  local p = vec(pos)
  step = step or 0.25
  local cols, rows = math.max(1, math.floor(width / step + 0.5)), math.max(1, math.floor(depth / step + 0.5))
  local x0, y0 = p.X - width / 2 + step / 2, p.Y - depth / 2 + step / 2
  local out = {step = step, cols = cols, rows = rows, x0 = x0, y0 = y0, z = {}}
  local reach = opts.reach or 5
  for j = 1, rows do
    local row = {}
    for i = 1, cols do
      local x, y = x0 + (i - 1) * step, y0 + (j - 1) * step
      if opts.top then
        local hits = w3.world.cast({X = x, Y = y, Z = p.Z + reach}, {X = x, Y = y, Z = p.Z - reach}, opts)
        row[i] = hits[1] and hits[1].pos.Z or false
      else
        local f = floor_near(x, y, p.Z, reach, opts)
        row[i] = f and f.z or false
      end
    end
    out.z[j] = row
  end
  return out
end

w3.scene = {}
local scene_found = function() end  -- w3.scene.time's remembered player (set further down)
-- plays a scene (a .w2scene path or resource) from one of its inputs; the scene finds its actors by their tags.
-- opts = {at = seconds, paused = true}: the first section starts at that second, or stands paused from its start.
-- The seek waits for the first tick on which the new scene's section is loaded and current.
function w3.scene.play(scene, input, opts)
  if type(scene) == "string" then scene = w3.load(scene) or error("no scene " .. scene) end
  local before = {}
  if opts then
    for _, p in ipairs(w3.all("CStoryScenePlayer")) do before[w3.addr(p)] = true end
  end
  game():GetStorySceneSystem():PlayScene(scene, input or "Input")
  if not opts or (not opts.at and not opts.paused) then return end
  local h
  local started = w3.time()
  h = w3._as("tw3se", w3.on_tick, function()
    if w3.time() - started > 20 then
      w3.off_tick(h)
      print("w3.scene.play: the scene did not reach its first section within 20 s, it plays from its start")
      return
    end
    for _, p in ipairs(w3.all("CStoryScenePlayer")) do
      if not before[w3.addr(p)] and pcall(w3._scene_time, p) then
        scene_found(p)
        if opts.paused then pcall(w3._scene_pause, p, true) end
        if not opts.at or pcall(w3._scene_seek, p, opts.at) then
          w3.off_tick(h)
        elseif opts.paused then
          pcall(w3._scene_pause, p, false)
        end
        return
      end
    end
  end)
end
function w3.scene.playing() return game():IsCurrentlyPlayingNonGameplayScene() end
-- Ends running scenes (docs/engine/scenes.md): every one, or one CStoryScenePlayer, at the next tick (a scene's
-- own event or choice handler runs inside its update). An actor: the game's StopAllScenes on it, which ends its
-- gameplay scenes only. Returns how many scenes it ends.
function w3.scene.stop(target)
  if target ~= nil and w3.classof(target) ~= "CStoryScenePlayer" then
    target:StopAllScenes()
    return 0
  end
  local players = target and {target} or w3.all("CStoryScenePlayer")
  w3._as("tw3se", w3.after, 0, function()
    for _, p in ipairs(players) do
      if w3.valid(p) then
        local ok, err = pcall(w3._scene_stop, p)
        if not ok then print("w3.scene.stop: " .. tostring(err)) end
      end
    end
  end)
  return #players
end
)TW3SELIB" R"TW3SELIB(
-- The game's picture in a rectangle of the screen (an editor's viewport): the whole camera frame scaled into
-- it at its own aspect, black around it; the overlay and the in-game UI stay full screen. Screen pixels.
w3.view = {}
function w3.view.rect(x, y, w, h)  -- (x, y, w, h) on, (false) off, () where the picture lies: x, y, w, h or nil
  if x == false then w3._view_rect(false) return nil end
  if x ~= nil then w3._view_rect(x, y, w, h) end
  local px, py, pw, ph = w3._view_rect()
  if px then return px, py, pw, ph end
  return nil
end
function w3.view.to_screen(pos)  -- a world position in screen pixels where it is drawn now, or nil off the picture
  local u, v = w3.camera.to_screen(pos)
  if not u then return nil end
  local r = {w3._view_rect()}
  if r[1] then return r[1] + u * r[3], r[2] + v * r[4] end
  return u * r[2], v * r[3]  -- off: nil, then the screen's width and height
end
-- An animation of a figure's sets at second t (for a scene preview and an animation library): through the
-- game's own sampler. sample gives the pose like w3.pose; pose lays a blend of animations on the figure every frame,
-- inside or outside a scene, until pose_clear.
local function animated(x)
  if w3.classof(x) and x.GetRootAnimatedComponent then
    local ok, c = pcall(function() return x:GetRootAnimatedComponent() end)
    if ok and c then return c end
  end
  return x
end
function w3.anim.sample(actor, name, t)  -- {count, duration, modelToWorld, bones}, or nil and why ("" = not loaded yet)
  return w3._anim_sample(animated(actor), name, t or 0)
end
function w3.anim.pose(actor, layers)  -- layers = {{name, t, weight}, ...}; again with new times moves it
  return w3._anim_pose(animated(actor), layers)
end
function w3.anim.pose_clear(actor)
  return w3._anim_pose(animated(actor), {})
end
-- The time of a running scene (for a scene preview with a playhead): where it stands, pause, seek forwards and
-- backwards in the current section. The scene is the first running one, or the CStoryScenePlayer given last.
-- The search over every object costs about 32 ms (two tick handlers with it run the game at 26 fps), so the
-- player found last stays while it lives and has not stopped, and with no scene the search runs at most every 0.5 s.
-- w3.scene.play with opts hands its new scene over here.
local lastPlayer, lastSearch = nil, -math.huge
local function scene_player(p)
  if p then return p end
  if lastPlayer and w3.valid(lastPlayer) and pcall(w3._scene_time, lastPlayer) then return lastPlayer end
  lastPlayer = nil
  local now = w3.time()
  if now - lastSearch < 0.5 then return nil end
  lastSearch = now
  for _, q in ipairs(w3.all("CStoryScenePlayer")) do
    if pcall(w3._scene_time, q) then lastPlayer = q return q end
  end
  return nil
end
scene_found = function(p) lastPlayer, lastSearch = p, -math.huge end
function w3.scene.time(p)  -- {section = name, t = seconds in the section, length = seconds}, or nil
  p = scene_player(p)
  if not p then return nil end
  local ok, t, length, sec = pcall(w3._scene_time, p)
  if not ok then return nil end
  local name = ""
  if sec and sec ~= 0 then
    local okn, n = pcall(function() return w3.object(sec, "CStorySceneSection").sectionName end)
    if okn and n then name = n end
  end
  return {section = name, t = t, length = length}
end
-- the current section's elements (lines, pauses, choices) in order: {index, start, duration, t, running}, and the
-- index of the current one; nil when no section plays
function w3.scene.elements(p)
  p = scene_player(p)
  if not p then return nil end
  local ok, els, cur = pcall(w3._scene_elements, p)
  if not ok then return nil end
  return els, cur
end
function w3.scene.pause(on, p)  -- people, camera, events and voice stand in their frame; false goes on
  p = scene_player(p) or error("no scene runs", 2)
  return w3._scene_pause(p, on ~= false)
end
function w3.scene.seek(t, p)  -- to second t of the current section, forwards and backwards, paused or playing
  p = scene_player(p) or error("no scene runs", 2)
  return w3._scene_seek(p, t)
end
function w3.scene.inputs_tags(scene)  -- the position tags a scene asks for
  if type(scene) == "string" then scene = w3.load(scene) or error("no scene " .. scene) end
  return scene:GetRequiredPositionTags() or {}
end
)TW3SELIB" R"TW3SELIB(
-- CVisualDebug made visible --------------------------------------------------------------------------------------------
-- The retail build drops the step in which figures hand their debug shapes to the renderer (SHOW_VisualDebug is
-- in the game's own show list and nothing shows). w3.debug.show(true) hooks CVisualDebug's Add/Remove natives,
-- keeps every shape with its timeout and draws it with the overlay: the game's own debug calls and those of mods.
w3.debug = {}
local dbg = {shapes = {}, hooks = nil, draw = nil}
local function colour(c)  -- a Color struct (or nil) as 0xRRGGBBAA
  if type(c) ~= "userdata" and type(c) ~= "table" then return 0xFF4040FF end
  local ok, r, g, b, a = pcall(function() return c.Red, c.Green, c.Blue, c.Alpha end)
  if not ok or not r then return 0xFF4040FF end
  if (a or 0) == 0 then a = 255 end
  return (r << 24) | (g << 16) | (b << 8) | a
end
local function where(self, pos, absolute)  -- a relative position is relative to the entity the debug belongs to
  local p = pos and vec(pos) or {X = 0, Y = 0, Z = 0, W = 1}
  if absolute then return p end
  local ok, owner = pcall(function() return self:GetParent() end)
  if ok and owner then
    local ok2, o = pcall(function() return owner:GetWorldPosition() end)
    if ok2 and o then return {X = o.X + p.X, Y = o.Y + p.Y, Z = o.Z + p.Z, W = 1} end
  end
  return p
end
local function keep(self, kind, name, shape, timeout)
  shape.kind = kind
  shape.expires = (timeout and timeout > 0) and (w3.time() + timeout) or nil
  dbg.shapes[tostring(w3.addr(self)) .. kind .. tostring(name)] = shape
end
local function drop(self, kind, name) dbg.shapes[tostring(w3.addr(self)) .. kind .. tostring(name)] = nil end
local function ring(c, r, axis, col)  -- a circle of 16 segments around c, in the plane normal to axis
  local prev
  for i = 0, 16 do
    local a = i / 16 * math.pi * 2
    local s, k = math.sin(a) * r, math.cos(a) * r
    local q = axis == "z" and {X = c.X + s, Y = c.Y + k, Z = c.Z} or axis == "x" and {X = c.X, Y = c.Y + s, Z = c.Z + k}
              or {X = c.X + s, Y = c.Y, Z = c.Z + k}
    if prev then w3.draw.line_at(prev, q, col, 2) end
    prev = q
  end
end
local function draw_shapes()
  local now = w3.time()
  for key, s in pairs(dbg.shapes) do
    if s.expires and now > s.expires then
      dbg.shapes[key] = nil
    elseif s.kind == "sphere" then
      ring(s.pos, s.radius, "z", s.col) ring(s.pos, s.radius, "x", s.col) ring(s.pos, s.radius, "y", s.col)
    elseif s.kind == "line" or s.kind == "arrow" then
      w3.draw.line_at(s.a, s.b, s.col, 2)
      if s.kind == "arrow" then w3.draw.marker_at(s.b, 6, s.col) end
    elseif s.kind == "box" then
      local c, h, yaw = s.pos, s.size, math.rad(s.yaw or 0)
      local function corner(x, y, z)
        local px, py = x * h.X / 2, y * h.Y / 2
        return {X = c.X + px * math.cos(yaw) - py * math.sin(yaw), Y = c.Y + px * math.sin(yaw) + py * math.cos(yaw),
                Z = c.Z + z * h.Z / 2}
      end
      local e = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}
      for _, ij in ipairs({{1,2},{2,3},{3,4},{4,1},{5,6},{6,7},{7,8},{8,5},{1,5},{2,6},{3,7},{4,8}}) do
        local a, b = e[ij[1]], e[ij[2]]
        w3.draw.line_at(corner(a[1], a[2], a[3]), corner(b[1], b[2], b[3]), s.col, 2)
      end
    elseif s.kind == "text" then
      w3.draw.text_at(s.pos, s.text, s.col)
    elseif s.kind == "axis" then
      local p, l = s.pos, s.scale or 1
      w3.draw.line_at(p, {X = p.X + l, Y = p.Y, Z = p.Z}, 0xFF0000FF, 2)
      w3.draw.line_at(p, {X = p.X, Y = p.Y + l, Z = p.Z}, 0x00FF00FF, 2)
      w3.draw.line_at(p, {X = p.X, Y = p.Y, Z = p.Z + l}, 0x4080FFFF, 2)
    elseif s.kind == "bar" then
      w3.draw.rect(s.x, s.y, s.w, s.h, 0x000000A0)
      w3.draw.rect(s.x, s.y, s.w * math.max(0, math.min(1, s.p or 0)), s.h, s.col)
      if s.text and s.text ~= "" then w3.draw.text(s.x + 4, s.y + 2, 0xFFFFFFFF, s.text) end
    end
  end
end
local function H(fn_name, handler)
  return w3.hook("CVisualDebug", fn_name, function(self, args, orig) pcall(handler, self, args) end)
end
function w3.debug.show(on)
  if on and not dbg.hooks then
    dbg.hooks = {
      H("AddSphere", function(self, a) keep(self, "sphere", a[1], {pos = where(self, a[3], a[4]), radius = a[2], col = colour(a[5])}, a[6]) end),
      H("AddText", function(self, a) keep(self, "text", a[1], {pos = where(self, a[3], a[4]), text = a[2], col = colour(a[6])}, a[8]) end),
      H("AddBox", function(self, a)
        local rot = a[4]
        keep(self, "box", a[1], {pos = where(self, a[3], a[5]), size = vec(a[2]), yaw = rot and rot.Yaw or 0, col = colour(a[6])}, a[7])
      end),
      H("AddAxis", function(self, a) keep(self, "axis", a[1], {pos = where(self, a[3], a[5]), scale = a[2]}, a[6]) end),
      H("AddLine", function(self, a) keep(self, "line", a[1], {a = where(self, a[2], a[4]), b = where(self, a[3], a[4]), col = colour(a[5])}, a[6]) end),
      H("AddArrow", function(self, a) keep(self, "arrow", a[1], {a = where(self, a[2], a[6]), b = where(self, a[3], a[6]), col = colour(a[7])}, a[9]) end),
      H("AddBar", function(self, a) keep(self, "bar", a[1], {x = a[2], y = a[3], w = a[4], h = a[5], p = a[6], col = colour(a[7]), text = a[8]}, a[9]) end),
      H("RemoveText", function(self, a) drop(self, "text", a[1]) end),
      H("RemoveSphere", function(self, a) drop(self, "sphere", a[1]) end),
      H("RemoveBox", function(self, a) drop(self, "box", a[1]) end),
      H("RemoveAxis", function(self, a) drop(self, "axis", a[1]) end),
      H("RemoveLine", function(self, a) drop(self, "line", a[1]) end),
      H("RemoveArrow", function(self, a) drop(self, "arrow", a[1]) end),
      H("RemoveBar", function(self, a) drop(self, "bar", a[1]) end),
    }
    dbg.draw = w3.on_draw(draw_shapes)
  elseif on == false and dbg.hooks then
    for _, h in ipairs(dbg.hooks) do w3.unhook(h) end
    if dbg.draw then w3.off_draw(dbg.draw) end
    dbg.hooks, dbg.draw, dbg.shapes = nil, nil, {}
  end
  return dbg.hooks ~= nil
end
function w3.debug.count() local n = 0 for _ in pairs(dbg.shapes) do n = n + 1 end return n end
-- w3.debug.watch(label, fn): "label: value" over the game every frame, fn() gives the value (a number shows with two
-- decimals); w3.debug.unwatch(h). The lines stand in a box below the top left corner; a mod taken out loses its own.
do
  local watches, order, drawing, nextWatch = {}, {}, false, 0
  local function shown(v)
    if type(v) == "number" and math.type(v) == "float" then return string.format("%.2f", v) end
    if type(v) == "table" and v.X and v.Y then return string.format("%.1f %.1f %.1f", v.X, v.Y, v.Z or 0) end
    return tostring(v)
  end
  local function draw()
    if #order == 0 then return end
    local lines, wide = {}, 0
    for _, h in ipairs(order) do
      local w = watches[h]
      local ok, v = pcall(w.fn)
      local line = w.label .. ": " .. (ok and shown(v) or ("error " .. tostring(v)))
      lines[#lines + 1] = line
      local lw = w3.draw.size(line)
      if lw > wide then wide = lw end
    end
    local _, lh = w3.draw.size("Ag")
    if lh <= 0 then return end
    w3.draw.rect(16, 64, wide + 20, #lines * (lh + 2) + 12, 0x101012C8)
    for i, l in ipairs(lines) do w3.draw.text(26, 70 + (i - 1) * (lh + 2), 0xE8E8E8FF, l) end
  end
  function w3.debug.watch(label, fn)
    if type(fn) ~= "function" then error("w3.debug.watch(label, fn): fn gives the value", 2) end
    if not drawing then
      drawing = true
      w3._as("tw3se", w3.on_draw, draw)
    end
    nextWatch = nextWatch + 1
    watches[nextWatch] = {label = tostring(label), fn = fn, owner = w3._mod()}
    order[#order + 1] = nextWatch
    return nextWatch
  end
  function w3.debug.unwatch(h)
    watches[h] = nil
    for i = #order, 1, -1 do if order[i] == h then table.remove(order, i) end end
  end
  function w3._watch_drop(owner)  -- a mod taken out (w3._events_drop)
    local n = 0
    for h, w in pairs(watches) do
      if w.owner == owner then w3.debug.unwatch(h) n = n + 1 end
    end
    return n
  end
end

-- The quest graph, live -----------------------------------------------------------------------------------------------
function w3.quest.threads() return w3.all("CQuestThread") end   -- every running quest thread (and its sub-threads)
-- the active blocks: {block, class, thread} of every thread, or of one
function w3.quest.blocks(thread)
  local out = {}
  for _, t in ipairs(thread and {thread} or w3.quest.threads()) do
    for _, b in ipairs(w3._quest_blocks(t)) do out[#out + 1] = {block = b, class = w3.classof(b), thread = t} end
  end
  return out
end
function w3.quest.children(thread) return w3._quest_children(thread) end
function w3.quest.activate(thread, block) return w3._quest_activate(thread, block) end   -- the block of that graph

-- Texts by string id, dialogue lines and choices --------------------------------------------------------------------
w3.text = {}
w3.text.set = w3._text_set                       -- w3.text.set(id, text); nil gives the game's text back
function w3.text.get(id) return w3.call("GetLocStringById", id) end
function w3.text.clear(id) return w3._text_set(id, nil) end

local function dialog() return w3.hud.module("DialogModule") or error("no dialogue module (main menu)") end
function w3.scene.choices()  -- the choices on screen now: {text, disabled}
  local out = {}
  for _, c in ipairs(dialog().lastSetChoices:to_table()) do
    out[#out + 1] = {text = c.description, disabled = c.disabled}
  end
  return out
end
function w3.scene.choose(i) game():GetStorySceneSystem():SendSignal("SSST_Accept", i - 1) end   -- 1 = the first

-- the line on screen: fn(text) returns the text to show instead (nil: as it is); the voice stays the game's
local lineFns, lineHook = {}, nil
function w3.scene.on_line(fn)
  lineFns[#lineFns + 1] = fn
  if not lineHook then
    lineHook = w3.hook("CR4HudModuleDialog", "OnDialogSentenceSet", function(self, args, orig)
      for _, f in ipairs(lineFns) do
        local ok, t = pcall(f, args[1])
        if ok and type(t) == "string" then args[1] = t end
      end
    end)
  end
  return #lineFns
end

-- a choice of its own in every dialogue whose choices pass `when(choices)` (nil: every one). Picked, fn() runs; when it
-- returns a number, that choice of the game's is taken (1 = the first), else the choices stay on screen.
-- The UI calls the module's OnDialogOptionAccepted event past every script hook, so the pick is caught one step down:
-- the event hands the index to the scene system's native SendSignal, which a hook does see.
-- The module gets the game's list once more with ours appended (its own call does the formatting, so every text is
-- wrapped once). After a pick of ours the UI has hidden the choices and waits for the scene; the list goes out again.
local extra, choiceHooks, gameCount = {}, nil, 0
local choiceFns = {}                                          -- w3.scene.on_choice: fn(text) -> the text shown
local last, sending = nil, false                              -- the game's last list {choices, alternativeUI}
local function send_choices(module)
  local list = {}
  for i, c in ipairs(last.choices) do list[i] = c end
  for _, x in ipairs(extra) do
    x.slot = nil
    if not x.when or x.when(last.shown) then
      list[#list + 1] = {description = x.text, emphasised = false, previouslyChoosen = false, disabled = false}
      x.slot = #list
    end
  end
  sending = true
  local ok, e = pcall(function() module:OnDialogChoicesSet(list, last.alt) end)
  sending = false
  if not ok then error(e, 0) end
end
local function install_choice_hooks()
  if choiceHooks then return end
  choiceHooks = {
    w3.hook("CR4HudModuleDialog", "OnDialogChoicesSet", function(self, args, orig)
      if sending then return orig() end                       -- our own call: the game's body formats and shows it
      for _, x in ipairs(extra) do x.slot = nil end
      local choices = args[1] and args[1]:to_table() or {}   -- copies; an empty list arrives as nil
      gameCount = #choices
      local renamed = false
      for _, c in ipairs(choices) do
        for _, f in ipairs(choiceFns) do
          local ok, t = pcall(f, c.description)
          if ok and type(t) == "string" and t ~= c.description then c.description = t renamed = true end
        end
      end
      if gameCount == 0 or (#extra == 0 and not renamed) then last = nil return orig() end   -- nothing to add or rename
      local shown = {}
      for i, c in ipairs(choices) do shown[i] = {text = c.description, disabled = c.disabled} end
      last = {choices = choices, alt = args[2], shown = shown}
      send_choices(self)
      return true                                             -- the engine's call is answered by ours
    end),
    w3.hook("CStorySceneSystem", "SendSignal", function(self, args, orig)
      local index = args[2] + 1
      for _, x in ipairs(extra) do
        if x.slot == index then
          if args[1] == "SSST_Accept" then
            local ok, r = pcall(x.fn)
            if ok and type(r) == "number" and r >= 1 and r <= gameCount then
              w3.scene.choose(r)
            elseif last then                                  -- the choices back on screen, once the UI has hidden them
              w3.after(0.1, function() if last then send_choices(dialog()) end end)
            end
          end
          return true                                          -- the scene never hears of this index
        end
      end
      orig()
    end),
  }
end
function w3.scene.add_choice(text, fn, when)
  install_choice_hooks()
  extra[#extra + 1] = {text = text, fn = fn, when = when}
  return #extra
end
-- the text of the dialogue's choices: fn(text) returns the text to show instead (nil: as it is)
function w3.scene.on_choice(fn)
  install_choice_hooks()
  choiceFns[#choiceFns + 1] = fn
  return #choiceFns
end
function w3.scene.remove_choices()
  extra, last, choiceFns = {}, nil, {}
  if choiceHooks then for _, h in ipairs(choiceHooks) do w3.unhook(h) end choiceHooks = nil end
  if lineHook then w3.unhook(lineHook) lineHook = nil lineFns = {} end
end
)TW3SELIB" R"TW3SELIB(
-- The TW3SE Menu's mod list: a mod's entry in the Mods tab, its settings and, if it wants, a tab of its own. The values are
-- kept in tw3se\vars.txt ("mod/<name>/<key>"). w3.mod{...} gives back the settings: cfg.level reads one,
-- cfg.level = 20 sets it (saved, shown in the menu, on_change runs). lua\lib\modmanager.lua draws them.
w3._modreg = w3._modreg or {}  -- by name: {info, settings, values, owner}
local SETTING_TYPES = {bool = true, slider = true, number = true, choice = true, text = true, key = true,
                       header = true, button = true}
local function setting_type(s)
  if s.type then return s.type end
  local d = s.default
  if type(d) == "boolean" then return "bool" end
  if type(d) == "number" then return (s.min and s.max) and "slider" or "number" end
  if s.items then return "choice" end
  return "text"
end
local function setting_value(s, v)  -- the value as the setting keeps it, or nil when it does not fit
  local t = s.type
  if t == "bool" then
    if type(v) == "boolean" then return v end
    if v == "true" then return true elseif v == "false" then return false end
    return nil
  end
  if t == "slider" or t == "number" then
    local n = tonumber(v)
    if not n or n ~= n then return nil end
    if s.min and n < s.min then n = s.min end
    if s.max and n > s.max then n = s.max end
    if s.int then n = math.floor(n + 0.5) end
    return n
  end
  if t == "choice" then
    for _, it in ipairs(s.items) do if it == v then return v end end
    return nil
  end
  if v == nil then return nil end
  return tostring(v)
end
local function stored(v)
  if type(v) == "number" then return math.type(v) == "integer" and tostring(v) or string.format("%.9g", v) end
  return tostring(v)
end
local function find_setting(reg, key)
  for _, s in ipairs(reg.settings) do if s.key == key then return s end end
end
function w3._mod_set(name, key, v)  -- (the menu and cfg.key = v) sets a value: kept, then the mod's on_change
  local reg = w3._modreg[name] or error("no mod " .. tostring(name), 2)
  local s = find_setting(reg, key) or error(name .. " has no setting " .. tostring(key), 2)
  local val = setting_value(s, v)
  if val == nil then error(name .. "." .. key .. ": " .. tostring(v) .. " does not fit a " .. s.type .. " setting", 2) end
  local old = reg.values[key]
  if old == val then return val end
  reg.values[key] = val
  w3._var_set("mod/" .. name .. "/" .. key, stored(val))
  if reg.info.on_change then
    local ok, err = pcall(reg.info.on_change, key, val, old)
    if not ok then print("mod " .. name .. ": on_change: " .. tostring(err)) end
  end
  return val
end
function w3._mod_drop(owner)  -- the mod is taken out: its entry goes (its kept values stay)
  for name, reg in pairs(w3._modreg) do if reg.owner == owner then w3._modreg[name] = nil end end
end
-- w3.store([name]): a table of the mod's that stays across saves and new games, in tw3se\store\<name>.lua, written
-- within 2 s of a change (the same values as w3.saved: numbers, strings, booleans, tables). It stays the same table
-- when the mod loads again. Outside a mod it needs a name.
do
  local stores, last, watching = {}, {}, false
  local DIR = w3.dir() .. "\\store"
  local function file(name) return DIR .. "\\" .. name:gsub("[^%w%-_]", "_") .. ".lua" end
  function w3.store(name)
    name = name or w3._mod()
    if name == "" then error("w3.store needs a name outside a mod", 2) end
    name = name:gsub("^mods/", ""):gsub("^autorun/", "")
    if stores[name] then return stores[name] end
    local t = {}
    local text = w3.fs.read(file(name))
    if text then
      local f, err = load(text, "=store/" .. name, "t", {})
      local ok, v = false, err
      if f then ok, v = pcall(f) end
      if ok and type(v) == "table" then t = v else print("w3.store: " .. file(name) .. ": " .. tostring(v)) end
    end
    stores[name], last[name] = t, w3._serialize(t)
    if not watching then
      watching = true
      w3._as("tw3se", w3.every, 2, function()
        for n, tt in pairs(stores) do
          local ok, s = pcall(w3._serialize, tt)
          if not ok then print("w3.store " .. n .. ": " .. tostring(s))
          elseif s ~= last[n] then
            w3.fs.mkdir(DIR)
            if w3.fs.write(file(n), "return " .. s) then last[n] = s end
          end
        end
      end)
    end
    return t
  end
end
-- w3.version(): TW3SE's version as text and as three numbers ("1.2.0", 1, 2, 0)
function w3.version()
  local v = w3._tw3se()
  local a, b, c = v:match("^(%d+)%.(%d+)%.(%d+)")
  return v, tonumber(a), tonumber(b), tonumber(c)
end
-- w3.lang(): the game's text language and voice language as its scripts name them ("DE", "EN"; GetCurrentTextLocCode,
-- GetGameLanguageName). w3.tr(v): v itself, or for a table by language ({EN = "Wolf level", DE = "Wolfsstufe"}) the
-- text of the game's language, else EN, else any.
local langText, langVoice, langAt = nil, nil, -10
function w3.lang()
  if w3.time() - langAt > 2 then
    langAt = w3.time()
    local ok, t = pcall(w3.call, "GetCurrentTextLocCode")
    if ok and type(t) == "string" and t ~= "" then langText = t:upper() end
    local okv, voice = pcall(function() return w3.global("theGame"):GetGameLanguageName("", "") end)
    if okv and type(voice) == "string" and voice ~= "" then langVoice = voice:upper() end
  end
  return langText or "EN", langVoice or langText or "EN"
end
function w3.tr(v)
  if type(v) ~= "table" then return v end
  local l = w3.lang()
  if v[l] ~= nil then return v[l] end
  if v.EN ~= nil then return v.EN end
  local _, any = next(v)
  return any
end

local function older_than(want)  -- TW3SE is older than the version "x.y.z" a mod needs
  local _, a, b, c = w3.version()
  local x, y, z = tostring(want):match("^(%d+)%.?(%d*)%.?(%d*)")
  if not x then error("w3.mod{tw3se = ...}: a version like \"1.2.0\"", 3) end
  local need = {tonumber(x), tonumber(y) or 0, tonumber(z) or 0}
  for i, have in ipairs({a, b, c}) do
    if have ~= need[i] then return have < need[i] end
  end
  return false
end

function w3.mod(info)
  if type(info) ~= "table" or type(info.name) ~= "string" or info.name == "" then
    error("w3.mod{name = ...}: a name is needed", 2)
  end
  if info.tw3se and older_than(info.tw3se) then
    error(info.name .. " needs TW3SE " .. info.tw3se .. " or newer (this is " .. w3.version() .. ")", 2)
  end
  local owner = w3._mod()
  local had = w3._modreg[info.name]
  if had and had.owner ~= owner then error("w3.mod: the name " .. info.name .. " is taken by " .. had.owner, 2) end
  local reg = {info = info, settings = {}, values = {}, owner = owner}
  for i, s in ipairs(info.settings or {}) do
    if type(s) ~= "table" then error("w3.mod: setting " .. i .. " is not a table", 2) end
    local c = {}
    for k, v in pairs(s) do c[k] = v end
    c.type = setting_type(c)
    if not SETTING_TYPES[c.type] then error("w3.mod: setting " .. i .. ": no type " .. tostring(c.type), 2) end
    if c.type ~= "header" and c.type ~= "button" then
      if type(c.key) ~= "string" then error("w3.mod: setting " .. i .. " needs a key", 2) end
      if c.type == "choice" and (type(c.items) ~= "table" or #c.items == 0) then
        error("w3.mod: setting " .. c.key .. ": a choice needs items", 2)
      end
      if c.type == "slider" and not (c.min and c.max) then error("w3.mod: setting " .. c.key .. ": a slider needs min and max", 2) end
      if c.default == nil then
        c.default = ({bool = false, slider = c.min, number = 0, choice = c.items and c.items[1], text = "", key = ""})[c.type]
      end
      c.default = setting_value(c, c.default)
      if c.default == nil then error("w3.mod: setting " .. c.key .. ": its default does not fit", 2) end
      -- The store has every value, a default too, so WitcherScript reads it with TW3SE_GetVar("mod/<name>/<key>").
      -- Beside it the default it was written with: while the two are the same the player changed nothing, and a new
      -- default of the mod takes its place.
      local path = "mod/" .. info.name .. "/" .. c.key
      local kept, keptDefault = w3._var_get(path), w3._var_get(path .. ".default")
      local own = nil  -- the player's own value (false is one too)
      if kept ~= nil and kept ~= keptDefault then own = setting_value(c, kept) end
      if own ~= nil then
        reg.values[c.key] = own
      else
        reg.values[c.key] = c.default
        w3._var_set(path, stored(c.default))
      end
      w3._var_set(path .. ".default", stored(c.default))
    elseif c.type == "button" and type(c.run) ~= "function" then
      error("w3.mod: button " .. tostring(c.label) .. " needs run = function", 2)
    end
    reg.settings[#reg.settings + 1] = c
  end
  w3._modreg[info.name] = reg
  if w3._gamemenu and reg.owner ~= "tw3se" and info.game_menu ~= false then  -- its page in the game's options too
    local ok, err = pcall(w3._gamemenu, info.name, reg)
    if not ok then print("w3.mod " .. info.name .. ": the options menu page: " .. tostring(err)) end
  end
  local cfg = {}
  function cfg.reset(key)  -- one setting (or every one) back to its default
    for _, s in ipairs(reg.settings) do
      if s.key and (key == nil or key == cfg or s.key == key) then w3._mod_set(info.name, s.key, s.default) end
    end
  end
  return setmetatable(cfg, {
    __index = function(_, k) return reg.values[k] end,
    __newindex = function(_, k, v) w3._mod_set(info.name, k, v) end,
  })
end
-- TW3SE's own entry in the TW3SE Menu's Mods tab. Script lines: the game compiles its scripts with the line of each
-- statement (file and line in bug reports and script stacks, the debugger's breakpoints, w3.on_line); the store's
-- value is what hotreload.cpp reads at the next start. Menu key: the key that opens the menu, at once (ui.cpp reads
-- the store at the start; its default is console_key from tw3se.config).
do
  local version, linesNow = w3._tw3se()
  local reverting = false  -- the menu key goes back to the one before (on_change below)
  local _, keyDefault = w3._menu_key()
  w3.mod{
    name = "TW3SE", version = version, author = "MaximdeWinter",
    description = "The Witcher 3 Script Extender. Script lines are " .. (linesNow and "on" or "off") ..
                  " in this session.",
    settings = {
      {key = "menu_key", type = "key", default = keyDefault, label = "Menu key",
       tip = "Opens and closes the TW3SE Menu"},
      {key = "script_lines", default = true, label = "Script lines (from the next game start)",
       tip = "File and line in bug reports and script stacks, breakpoints in the debugger, w3.on_line. " ..
             "The game compiles its scripts again at the next start."},
      {key = "focus_at_start", default = true, label = "Game window in front at the start",
       tip = "Keyboard and controller work right after the start. A window you switched to while the game loaded " ..
             "keeps the focus"},
      {key = "mods_menu_fix", default = true, label = "Fix the game's Options > Mods (from the next game start)",
       tip = "Every mod's page opens. From the 11th entry on, a page sits in a submenu with one entry, Settings"},
    },
    on_change = function(key, value, old)
      if key == "menu_key" then
        -- a key the menu cannot take (a controller button: its combination is menu_pad) goes back to the one before
        local ok, err = pcall(w3._menu_key, value)
        if not ok and not reverting then
          print("TW3SE: " .. tostring(err):gsub("^.-:%d+: ", "") .. " (the controller opens the menu by menu_pad)")
          reverting = true  -- an old value that fails too (kept from before) stays: no ping-pong
          pcall(w3._mod_set, "TW3SE", "menu_key", old)
          reverting = false
        end
      elseif key == "script_lines" then
        print("TW3SE: script lines " .. (value and "on" or "off") .. " from the next game start")
      end
    end,
  }
  w3._modreg["TW3SE"].owner = "tw3se"  -- never taken out with a Lua mod
  -- A graphics option applied through the options (path tracing, ray tracing, upscalers ...) rebuilds the game's
  -- render resources at once. Blend sets must not live through that (path tracing switched with sets loaded crashed
  -- the game): they end before it, and w3.deform.on_reset(fn) tells the mods to load theirs again.
  -- (hooked at the first ticks: this block runs while the library loads, before the game has its names and classes;
  -- a hook made then left the game's name table locked and its start hung)
  -- One rebuild for many clicks: the options menu sets a value at every click, and the game rebuilt everything at
  -- each (frame generation switched on and off at speed, 52 rebuilds in a few seconds; the physics kept
  -- pointers into what the rebuilds destroyed and the game crashed seconds later, at another place each time, with
  -- the memory still held). A value of a rebuild group waits now: the menu reads it back at once (GetVarValue), the
  -- game takes the last value of each option when the clicks have paused `apply_after` s, an option back at its old
  -- value not at all, and all of them under one rebuild. Then the menu's own handler runs again for each, so the
  -- options that depend on it (DLSS Quality, frame generation greyed or not) see the game's new state. A save of the
  -- settings (leaving the options) takes what waits first.
  local rebuildGroups = {Graphics = true, PostProcess = true, Rendering = true}
  local function isRebuildGroup(group) return rebuildGroups[group] or group:find("^Rendering") ~= nil end
  local apply_after = 1.0
  local waiting, order, lastClick = {}, {}, 0  -- "group/var" -> {group, var, value, before}; the keys in click order
  local applying, replaying, menu, menuGroup = false, false, nil, {}  -- the options menu seen last, each option's group id there
  local realGet  -- the game's GetVarValue (not through the hook)
  local function apply(why)
    if applying or #order == 0 then return end
    local list = {}
    for _, k in ipairs(order) do
      local e = waiting[k]
      if e and e.value ~= e.before then list[#list + 1] = e end
    end
    waiting, order = {}, {}
    if #list == 0 then return end
    local ok, w = pcall(function() return w3.object(w3.addr(w3.global("theGame"):GetInGameConfigWrapper()), "CInGameConfigWrapper") end)
    if not ok or not w then return end
    applying = true
    w3._render_rebuild(0)  -- plugins let go of the game's render data (RegisterOnRenderRebuild), their sets too
    local n = w3._bs_reset_all()
    if n > 0 then
      for _, fn in ipairs(w3.deform._reset_fns) do pcall(w3._call_as, "tw3se", fn, n) end
    end
    for _, e in ipairs(list) do
      local okSet, err = pcall(function() w:SetVarValue(e.group, e.var, e.value) end)
      if not okSet then print("TW3SE: graphics option " .. e.var .. " not applied: " .. tostring(err)) end
    end
    w3._render_rebuild(1)  -- the rebuild is done (it runs inside SetVarValue): they take it again
    applying = false
    print(("TW3SE: graphics options: %d applied under one rebuild (%s)"):format(#list, why))
    local alive = false  -- (the menu still open: never a call into one that is gone)
    if menu and why == "the clicks paused" then
      for _, m in ipairs(w3.all("CR4IngameMenu")) do alive = alive or w3.addr(m) == w3.addr(menu) end
    end
    if alive then  -- the menu's handler again: what depends on them sees the new state
      -- (its SetVarValue then is no click: the values are in. A frame generation's virtual option reads back
      -- other than it was set, so the handler's call waited as a new change, and the menu rebuilt again each second)
      replaying = true
      for _, e in ipairs(list) do
        local gid = menuGroup[e.var]
        if gid then pcall(function() menu:OnOptionValueChanged(gid, e.var, e.value) end) end
      end
      replaying = false
    end
  end
)TW3SELIB" R"TW3SELIB(
  local configHooked = false
  local function hookConfig()
    configHooked = true
    pcall(w3.hook, "CInGameConfigWrapper", "GetVarValue", function(self, args, orig)
      local e = waiting[tostring(args and args[1] or "") .. "/" .. tostring(args and args[2] or "")]
      if e then return e.value end
      return orig()
    end)
    realGet = function(w, group, var)
      local ok, v = pcall(function() return w:GetVarValue(group, var) end)
      return ok and v or nil
    end
    -- `graphics_apply = live` in tw3se.config: as before, each click rebuilt at once (to compare)
    local live = w3._tw3se_config("graphics_apply", "bundled") == "live"
    pcall(w3.hook, "CInGameConfigWrapper", "SetVarValue", function(self, args, orig)
      local group = tostring(args and args[1] or "")
      if applying or not isRebuildGroup(group) then return orig() end
      if replaying then return end
      if live then
        w3._render_rebuild(0)
        local n = w3._bs_reset_all()
        if n > 0 then
          for _, fn in ipairs(w3.deform._reset_fns) do pcall(w3._call_as, "tw3se", fn, n) end
        end
        local r = table.pack(pcall(orig))
        w3._render_rebuild(1)
        if not r[1] then error(r[2], 0) end
        return table.unpack(r, 2, r.n)
      end
      local var, value = tostring(args[2] or ""), tostring(args[3] or "")
      local key = group .. "/" .. var
      local e = waiting[key]
      if not e then
        local before = realGet(self, group, var)
        if before == value then return end  -- (the same value again: no rebuild; the menu's handler run once more)
        e = {group = group, var = var, before = before}
        waiting[key] = e
        order[#order + 1] = key
      end
      e.value = value
      lastClick = w3.time()
    end)
    pcall(w3.hook, "CR4IngameMenu", "OnOptionValueChanged", function(self, args, orig)
      menu = self
      if args and args[2] ~= nil then menuGroup[tostring(args[2])] = args[1] end
      return orig()
    end)
    pcall(w3.hook, "CR4IngameMenu", "OnCloseMenu", function(self, args, orig)
      apply("the menu closes")
      menu = nil
      return orig()
    end)
    pcall(w3.hook, "CR4Game", "SaveUserSettings", function(self, args, orig)
      apply("the settings are saved")
      return orig()
    end)
    w3._as("tw3se", w3.on_tick, function()
      if #order > 0 and not applying and w3.time() - lastClick >= apply_after then apply("the clicks paused") end
    end)
  end
  w3._as("tw3se", w3.every, 2, function() if not configHooked then hookConfig() end end)
end
-- The default values of a class's fields, its parents' too: the fields of a fresh object, which goes
-- again at once. A script class gets its `default` lines that way, a native class its constructor's values. Structs
-- and arrays come as tables; a class w3.new cannot make (an entity, an abstract one) gives the reason.
function w3.defaults(class)
  local info = w3.class(class) or error("w3.defaults: no class " .. tostring(class), 2)
  local ok, o = pcall(w3.new, class)
  if not ok or not o then error("w3.defaults: " .. tostring(o):gsub("^.-:%d+: ", ""), 2) end
  local out = {}
  while info do
    for name in pairs(info.fields or {}) do
      if out[name] == nil then
        local okf, v = pcall(function() return o[name] end)
        if okf and type(v) == "userdata" then
          local okt, t = pcall(function() return v:to_table() end)
          v = okt and t or tostring(v)
        end
        if okf then out[name] = v end
      end
    end
    info = info.parent and w3.class(info.parent)
  end
  w3.discard(o)
  return out
end
function w3.mods()  -- the mods w3.mod made: {{name, version, author, description, owner}, ...} by name
  local out = {}
  for name, reg in pairs(w3._modreg) do
    out[#out + 1] = {name = name, version = reg.info.version, author = reg.info.author,
                     description = reg.info.description, owner = reg.owner}
  end
  table.sort(out, function(a, b) return a.name:lower() < b.name:lower() end)
  return out
end
)TW3SELIB" R"TW3SELIB(
-- The game's events for mods: w3.on(name, fn) runs fn(e) with a table e each time it happens, w3.off(h) stops
-- it. A hook or a watch goes on with the first function of an event and belongs to TW3SE (it stays when a mod is
-- taken out); each mod's function runs as that mod (w3._call_as: its error counts for it, the others go on).
-- Where each one comes from (the game's scripts, content0\scripts):
--   hit           W3DamageManagerProcessor.ProcessAction, after it: every damage action with its final damage
--   death         CActor.OnDeath (every actor class calls super.OnDeath)
--   item_added    CInventoryComponent.OnItemAdded; item_removed: OnItemRemoved (the name read before it)
--   level_up      W3LevelManager.GainLevel when it returns true
--   combat_start  OnCombatStart / OnCombatFinished of W3PlayerWitcher and W3ReplacerCiri (combat_end)
--   sign          W3PlayerWitcher.CastSign when it returns true; consume: W3PlayerWitcher.ConsumeItem
--   fact          the native FactsAdd
--   spawn         CNewNPC.OnSpawned
--   scene_start, scene_end, quest, world, hour: asked every 0.5 s / 2 s / 1 s / 1 s (scenes end in the player's
--                 states, so a watch is surer than a hook there)
do
  local subs = {}     -- event -> {[handle] = {fn, owner}}
  local of = {}       -- handle -> event
  local started = {}  -- event -> true once its hook or watch is on
  local nextHandle = 0

  local function emit(name, e)
    local list = subs[name]
    if not list or next(list) == nil then return end
    for _, s in pairs(list) do w3._call_as(s.owner, s.fn, e) end
  end
  local function busy(name) return subs[name] and next(subs[name]) ~= nil end

  local function as_tw3se(fn, ...) return w3._as("tw3se", fn, ...) end
  -- a hook that runs the original and hands its result to `after`: observe("Class", "Method", after) or
  -- observe("GlobalFunction", after)
  local function observe(a, b, c)
    local after = c or b
    local function handler(self, args, orig)
      local r = orig()
      local ok, err = pcall(after, self, args, r)
      if not ok then print("w3.on: " .. tostring(err)) end
      return r
    end
    if c then return as_tw3se(w3.hook, a, b, handler) end
    return as_tw3se(w3.hook, a, handler)
  end
  local function poll(seconds, step)
    as_tw3se(w3.every, seconds, function() pcall(step) end)
  end

  local starts = {}
  starts.hit = function()
    observe("W3DamageManagerProcessor", "ProcessAction", function(self, args)
      local a = args[1]
      if not busy("hit") or not a then return end
      local d = a.processedDmg
      emit("hit", {victim = a.victim, attacker = a.attacker, action = a,
                   damage = d and ((d.vitalityDamage or 0) + (d.essenceDamage or 0)) or 0})
    end)
  end
  starts.death = function()
    observe("CActor", "OnDeath", function(self, args)
      local a = args[1]
      emit("death", {victim = self, killer = a and a.attacker, action = a})
    end)
  end
  local function owner_of(inv)
    local ok, ent = pcall(function() return inv:GetEntity() end)
    return ok and ent or nil
  end
  starts.item_added = function()
    observe("CInventoryComponent", "OnItemAdded", function(self, args)
      if not busy("item_added") then return end
      local data, ent = args[1], owner_of(self)
      emit("item_added", {owner = ent, player = ent ~= nil and ent == w3.player(), name = data and data.itemName,
                          count = data and data.quantity, ids = data and data.ids})
    end)
  end
  starts.item_removed = function()  -- the name is read before the item goes
    as_tw3se(w3.hook, "CInventoryComponent", "OnItemRemoved", function(self, args, orig)
      local name
      if busy("item_removed") then pcall(function() name = self:GetItemName(args[1]) end) end
      local r = orig()
      if busy("item_removed") then
        local ent = owner_of(self)
        emit("item_removed", {owner = ent, player = ent ~= nil and ent == w3.player(), name = name, id = args[1],
                              count = args[2]})
      end
      return r
    end)
  end
  starts.level_up = function()
    observe("W3LevelManager", "GainLevel", function(self, args, r)
      if r then emit("level_up", {level = self:GetLevel()}) end
    end)
  end
  local function combat(name, event)
    return function()
      for _, class in ipairs({"W3PlayerWitcher", "W3ReplacerCiri"}) do
        observe(class, event, function(self) emit(name, {player = self}) end)
      end
    end
  end
  starts.combat_start = combat("combat_start", "OnCombatStart")
  starts.combat_end = combat("combat_end", "OnCombatFinished")
  starts.sign = function()
    observe("W3PlayerWitcher", "CastSign", function(self, args, r)
      if r then emit("sign", {sign = self.equippedSign}) end
    end)
  end
  starts.consume = function()  -- food and drinks: ConsumeItem; potions: DrinkPreparedPotion (never ConsumeItem)
    as_tw3se(w3.hook, "W3PlayerWitcher", "ConsumeItem", function(self, args, orig)
      local name
      if busy("consume") then pcall(function() name = self:GetInventory():GetItemName(args[1]) end) end
      local r = orig()
      if r and busy("consume") then emit("consume", {name = name, id = args[1]}) end
      return r
    end)
    -- (slot, optional itemId): the item, else the one on the slot. It returns nothing and leaves early (no potion,
    -- none left, too much toxicity): drunk is when its charges went down
    as_tw3se(w3.hook, "W3PlayerWitcher", "DrinkPreparedPotion", function(self, args, orig)
      if not busy("consume") then return orig() end
      local inv, id, name, before
      pcall(function()
        inv, id = self:GetInventory(), args[2]
        if not (id and inv:IsIdValid(id)) then
          local ok, onSlot = self:GetItemEquippedOnSlot(args[1], id)
          id = ok and onSlot or nil
        end
        if id then name, before = inv:GetItemName(id), inv:SingletonItemGetAmmo(id) end
      end)
      local r = orig()
      if name then
        local okA, after = pcall(function() return inv:IsIdValid(id) and inv:SingletonItemGetAmmo(id) or 0 end)
        if not okA or not before or after < before then emit("consume", {name = name, id = id, potion = true}) end
      end
      return r
    end)
  end
  starts.fact = function()
    observe("FactsAdd", function(self, args) emit("fact", {name = args[1], value = args[2] or 1}) end)
  end
  starts.spawn = function()
    observe("CNewNPC", "OnSpawned", function(self) emit("spawn", {npc = self}) end)
  end
  local scene = false
  local function scene_watch()
    poll(0.5, function()
      local now = w3.player() ~= nil and w3.scene.playing() or false
      if now ~= scene then
        scene = now
        emit(now and "scene_start" or "scene_end", {})
      end
    end)
  end
  starts.scene_start, starts.scene_end = scene_watch, scene_watch
  local quests
  starts.quest = function()
    poll(2, function()
      if not w3.player() then quests = nil return end
      local now = {}
      for _, q in ipairs(w3.quest.list()) do now[q.title] = q.status end
      if quests then
        for title, status in pairs(now) do
          if quests[title] ~= status then emit("quest", {title = title, status = status, old = quests[title]}) end
        end
      end
      quests = now
    end)
  end
  local world
  starts.world = function()
    poll(1, function()
      if not w3.player() then return end
      local now = w3.world.path()
      if world and now ~= world then emit("world", {path = now, old = world}) end
      world = now
    end)
  end
  local hour
  starts.hour = function()
    poll(1, function()
      if not w3.player() then return end
      local t = w3.time.get()
      if hour and t.hour ~= hour then emit("hour", {hour = t.hour, day = t.day}) end
      hour = t.hour
    end)
  end
  local shared = {scene_start = "scene", scene_end = "scene"}  -- one watch for both
  local other = {}  -- handle -> {off, h}: "load" and "save" go to w3.on_load / w3.on_save

  function w3.on(name, fn)
    if type(fn) ~= "function" then error("w3.on(name, fn): a function is needed", 2) end
    if name == "load" or name == "save" then
      local h = (name == "load" and w3.on_load or w3.on_save)(function() fn({}) end)
      nextHandle = nextHandle + 1
      other[nextHandle] = {off = name == "load" and w3.off_load or w3.off_save, h = h}
      return nextHandle
    end
    local start = starts[name]
    if not start then
      local names = {}
      for k in pairs(starts) do names[#names + 1] = k end
      table.sort(names)
      error("w3.on: no event '" .. tostring(name) .. "' (" .. table.concat(names, ", ") .. ", load, save)", 2)
    end
    local key = shared[name] or name
    if not started[key] then
      started[key] = true
      local ok, err = pcall(start)
      if not ok then
        started[key] = nil
        error("w3.on('" .. name .. "'): " .. tostring(err), 2)
      end
    end
    nextHandle = nextHandle + 1
    subs[name] = subs[name] or {}
    subs[name][nextHandle] = {fn = fn, owner = w3._mod()}
    of[nextHandle] = name
    return nextHandle
  end
  function w3.off(h)
    if other[h] then
      other[h].off(other[h].h)
      other[h] = nil
      return
    end
    local name = of[h]
    if name and subs[name] then subs[name][h] = nil end
    of[h] = nil
  end
  function w3._events_drop(owner)  -- a mod taken out: its event functions go (lua.cpp UnloadMod), its watches too
    local n = w3._watch_drop and w3._watch_drop(owner) or 0
    for h, name in pairs(of) do
      local s = subs[name] and subs[name][h]
      if s and s.owner == owner then
        subs[name][h], of[h] = nil, nil
        n = n + 1
      end
    end
    return n
  end
  function w3.events()  -- the names w3.on knows
    local names = {"load", "save"}
    for k in pairs(starts) do names[#names + 1] = k end
    table.sort(names)
    return names
  end
end
)TW3SELIB" R"TW3SELIB(
-- A Lua mod's settings in the game's own options menu: Options > Mods > <the mod's name>. w3.mod writes the
-- page as an XML of the game's (bin\config\r4game\user_config_matrix\pc\tw3se_<name>.xml, read at the game's start:
-- a new or changed page shows from the next start). Checkboxes are TOGGLE, sliders SLIDER, choices OPTIONS; number,
-- text, key and button stay in the TW3SE Menu. The menu asks GetLocStringByKeyExt for every label (igmOptions.ws: the
-- path's parts as is and with "panel_", an option as "option_<name>", a choice's item as "preset_value_<name>"); a hook
-- answers for TW3SE's keys. Once a second the values go both ways: a change in the game's menu sets the mod's value
-- (on_change runs), a change from Lua or the TW3SE Menu goes into the game's menu. w3.mod{game_menu = false} leaves
-- the page out.
do
  local CONFIG = w3.dir() .. "\\..\\..\\config\\r4game\\user_config_matrix\\pc"
  -- safe mode: no Lua mod loads, so none has a page, an item or an action; the clean-ups below would take every
  -- mod's files away, and the start after it would be without them (the game reads them before Lua runs)
  local SAFE = w3._tw3se ~= nil and select(3, w3._tw3se()) == true
  local texts = {}   -- localisation key -> a text, or a table by language
  local pages = {}   -- mod name -> {group, vars = {{s, id}}, last = {key -> the game's text}}
  local hooked, polling = false, false

  local function id_of(s) return (tostring(s):gsub("[^%w]", "")) end
  local function esc(s)
    return (tostring(s):gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"):gsub('"', "&quot;"))
  end
  local function game_text(s, v)  -- the mod's value as the game's menu keeps it
    if s.type == "bool" then return v and "true" or "false" end
    if s.type == "choice" then
      for i, it in ipairs(s.items) do if it == v then return tostring(i - 1) end end
      return "0"
    end
    if s.int or (v == math.floor(v)) then return tostring(math.floor(v + 0.5)) end
    return string.format("%.4g", v)
  end
  local function mod_value(s, text)  -- the game's text as the mod's value
    if s.type == "bool" then return text == "true" end
    if s.type == "choice" then return s.items[(tonumber(text) or 0) + 1] end
    return tonumber(text)
  end
  local function wrapper()
    return w3.object(w3.addr(w3.global("theGame"):GetInGameConfigWrapper()), "CInGameConfigWrapper")
  end

  -- The texts go to lockeys.cpp, which answers GetLocStringByKeyExt in C (a Lua hook would run Lua for every text
  -- the game looks up). w3.tr picks the language when a text goes there; the watch below sends them again when the
  -- game's language changes. When the C handler cannot go on (another build), a Lua hook answers.
  local inC, sentLang = nil, nil
  local function hook_texts()
    if hooked then return end
    hooked = true
    w3._as("tw3se", w3.hook, "GetLocStringByKeyExt", function(self, args, orig)
      local t = texts[args[1]]
      if t ~= nil then return w3.tr(t) end
      return orig()
    end)
  end
  local function send(key, text)
    if inC ~= false and w3._lockey then
      local ok, put = pcall(w3._lockey, key, tostring(w3.tr(text) or ""))
      inC = ok and put or false
    else
      inC = false
    end
    if inC == false then hook_texts() end
  end
  local langWatch = false
  local function resend_on_language()  -- once a second from the first text on (a mod with texts and no menu page too)
    local l = w3.lang()
    if sentLang and l ~= sentLang and inC then
      for k, t in pairs(texts) do if type(t) == "table" then send(k, t) end end
    end
    sentLang = l
  end
  function w3._loctext(key, text)  -- a text for a localisation key of TW3SE's (the menu pages, w3.items' names)
    texts[key] = text
    if not sentLang then sentLang = w3.lang() end
    send(key, text)
    if not langWatch then
      langWatch = true
      w3._as("tw3se", w3.every, 1, function() pcall(resend_on_language) end)
    end
  end

  local function sync()
    local ok, w = pcall(wrapper)
    if not ok or not w then return end
    local dirty = false
    for name, page in pairs(pages) do
      local reg = w3._modreg[name]
      if not reg then pages[name] = nil else
        for _, v in ipairs(page.vars) do
          local okv, g = pcall(function() return w:GetVarValue(page.group, v.id) end)
          if okv and type(g) == "string" and g ~= "" then
            local mine = game_text(v.s, reg.values[v.s.key])
            if page.last[v.id] == nil then page.last[v.id] = g end  -- the first look: the mod's value counts
            if g ~= page.last[v.id] then  -- changed in the game's menu
              page.last[v.id] = g
              if g ~= mine then
                local val = mod_value(v.s, g)
                if val ~= nil then pcall(w3._mod_set, name, v.s.key, val) end
              end
            elseif g ~= mine then  -- changed from Lua or the TW3SE Menu
              if pcall(function() w:SetVarValue(page.group, v.id, mine) end) then
                page.last[v.id] = mine
                dirty = true
              end
            end
          end
        end
      end
    end
    if dirty then pcall(function() w3.global("theGame"):SaveUserSettings() end) end
  end

  function w3._gamemenu(name, reg)
    local safe = id_of(name)
    if safe == "" then return end
    local group, seg = "TW3SE_" .. safe, "tw3se_" .. safe
    local vars, presets, lines = {}, {}, {}
    w3._loctext(seg, name)
    w3._loctext("panel_" .. seg, name)
    -- the page's preset row (the menu labels it "preset_" + the group's displayName): one value, the defaults
    w3._loctext("preset_Mods." .. seg, {EN = "Preset", DE = "Voreinstellung"})
    w3._loctext("preset_value_" .. seg .. "_default", {EN = "Default", DE = "Standard"})
    for i, s in ipairs(reg.settings) do
      local vid = group .. "_" .. (s.key and id_of(s.key) or ("h" .. i))
      local label = s.label or s.key or ""
      if s.type == "bool" or s.type == "slider" or s.type == "choice" then
        vars[#vars + 1] = {s = s, id = vid}
        presets[#presets + 1] = string.format('        <Entry varId="%s" value="%s"/>', vid, esc(game_text(s, s.default)))
        w3._loctext("option_" .. vid, label)
        if s.type == "bool" then
          lines[#lines + 1] = string.format('      <Var id="%s" displayName="%s" displayType="TOGGLE"/>', vid, vid)
        elseif s.type == "slider" then
          local steps = s.int and math.max(1, math.floor(s.max - s.min + 0.5)) or 100
          lines[#lines + 1] = string.format('      <Var id="%s" displayName="%s" displayType="SLIDER;%s;%s;%d"/>', vid,
                                            vid, game_text({}, s.min), game_text({}, s.max), steps)
        else
          local opts = {}
          for k, it in ipairs(s.items) do
            local oid = vid .. "_" .. (k - 1)
            w3._loctext("preset_value_" .. oid, it)
            opts[#opts + 1] = string.format('          <Option id="%d" displayName="%s"><Entry varId="%s" value="%d"/></Option>',
                                            k - 1, oid, vid, k - 1)
          end
          lines[#lines + 1] = string.format('      <Var id="%s" displayName="%s" displayType="OPTIONS">\n        <OptionsArray>\n%s\n        </OptionsArray>\n      </Var>',
                                            vid, vid, table.concat(opts, "\n"))
        end
      elseif s.type == "header" then
        w3._loctext("option_" .. vid, label)
        lines[#lines + 1] = string.format('      <Var id="%s" displayName="%s" displayType="SUBTLE_SEPARATOR"/>', vid, vid)
      end
    end
    if #vars == 0 then  -- only text, number, key or button: the TW3SE Menu has them
      pages[name] = nil  -- a page from before the mod loaded again goes (its settings are gone)
      return
    end
    pages[name] = {group = group, vars = vars, last = {}}
    local xml = table.concat({
      '<?xml version="1.0" encoding="utf-8"?>',
      '<!-- Written by TW3SE from the w3.mod settings of "' .. esc(name) .. '". It comes again at each start. -->',
      '<UserConfig>',
      '  <Group id="' .. group .. '" displayName="Mods.' .. seg .. '">',
      '    <PresetsArray>',
      '      <Preset id="0" displayName="' .. seg .. '_default">',
      table.concat(presets, "\n"),
      '      </Preset>',
      '    </PresetsArray>',
      '    <VisibleVars>',
      table.concat(lines, "\n"),
      '    </VisibleVars>',
      '  </Group>',
      '</UserConfig>', ''}, "\n")
    local file = CONFIG .. "\\tw3se_" .. safe .. ".xml"
    if w3.fs.read(file) ~= xml then
      local ok, err = w3.fs.write(file, xml)
      if not ok then print("w3.mod " .. name .. ": " .. file .. ": " .. tostring(err)) end
    end
    if not polling then
      polling = true
      w3._as("tw3se", w3.every, 1, function() pcall(sync) end)
    end
  end

)TW3SELIB" R"TW3SELIB(
  -- Items and abilities of mods (measured in the game): the game reads item definitions only through a DLC,
  -- a CR4DefinitionsDLCMounter in its .reddlc names the XML (a loose XML under dlc\ is never read). TW3SE keeps a
  -- fixed dlc\dlctw3se\dlctw3se.reddlc (DLCTW3SE below, made from dlc13's with tools\cr2w: one mounter on
  -- tw3se_items.xml) and writes every mod's w3.items into that one XML, in tw3se\files\dlc\dlctw3se\. TW3SE packs
  -- both at its next start (loose files); the game has the items from then on. An item's name and description go
  -- through lockeys (keys tw3se_item_<name>, tw3se_item_desc_<name>).
  w3.items = w3.items or {}
  local DLC = w3.dir() .. "\\files\\dlc\\dlctw3se"
  local ITEMS = DLC .. "\\data\\gameplay\\items"
  local defs = {}  -- mod id -> {items = {name -> def}, abilities = {name -> def}}

)TW3SELIB" R"TW3SELIB(
  local DLCTW3SE = "\x43\x52\x32\x57\xa4\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x7a\x03\x00\x00\z
    \x7a\x03\x00\x00\x5d\x1d\xf8\xa5\x06\x00\x00\x00\xa0\x00\x00\x00\x6a\x01\x00\x00\x0b\x4a\xe2\x99\x0a\x02\x00\x00\z
    \x14\x00\x00\x00\xa9\x31\x2a\x94\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xaa\x02\x00\x00\x01\x00\x00\x00\z
    \x55\x4b\xbb\xec\xba\x02\x00\x00\x02\x00\x00\x00\x9d\x3a\x55\xaa\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\z
    \x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\z
    \x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x43\x44\x4c\x43\x44\x65\x66\z
    \x69\x6e\x69\x74\x69\x6f\x6e\x00\x69\x64\x00\x43\x4e\x61\x6d\x65\x00\x64\x6c\x63\x5f\x30\x31\x33\x5f\x30\x30\x31\z
    \x00\x6c\x6f\x63\x61\x6c\x69\x7a\x65\x64\x4e\x61\x6d\x65\x4b\x65\x79\x00\x53\x74\x72\x69\x6e\x67\x00\x6c\x6f\x63\z
    \x61\x6c\x69\x7a\x65\x64\x44\x65\x73\x63\x72\x69\x70\x74\x69\x6f\x6e\x4b\x65\x79\x00\x6d\x6f\x75\x6e\x74\x65\x72\z
    \x73\x00\x61\x72\x72\x61\x79\x3a\x32\x2c\x30\x2c\x70\x74\x72\x3a\x49\x47\x61\x6d\x65\x70\x6c\x61\x79\x44\x4c\x43\z
    \x4d\x6f\x75\x6e\x74\x65\x72\x00\x43\x52\x34\x44\x65\x66\x69\x6e\x69\x74\x69\x6f\x6e\x73\x44\x4c\x43\x4d\x6f\x75\z
    \x6e\x74\x65\x72\x00\x64\x65\x66\x69\x6e\x69\x74\x69\x6f\x6e\x58\x6d\x6c\x46\x69\x6c\x65\x50\x61\x74\x68\x00\x43\z
    \x52\x34\x44\x65\x66\x69\x6e\x69\x74\x69\x6f\x6e\x73\x45\x6e\x74\x69\x74\x69\x65\x54\x65\x6d\x70\x6c\x61\x74\x65\z
    \x73\x44\x4c\x43\x4d\x6f\x75\x6e\x74\x65\x72\x00\x65\x6e\x74\x69\x74\x69\x65\x54\x65\x6d\x70\x6c\x61\x74\x65\x73\z
    \x44\x69\x72\x65\x63\x74\x6f\x72\x79\x50\x61\x74\x68\x00\x43\x52\x34\x53\x63\x61\x6c\x65\x66\x6f\x72\x6d\x43\x6f\z
    \x6e\x74\x65\x6e\x74\x44\x4c\x43\x4d\x6f\x75\x6e\x74\x65\x72\x00\x73\x63\x61\x6c\x65\x66\x6f\x72\x6d\x44\x69\x72\z
    \x65\x63\x74\x6f\x72\x79\x50\x61\x74\x68\x00\x43\x52\x34\x44\x65\x66\x69\x6e\x69\x74\x69\x6f\x6e\x73\x4e\x47\x50\z
    \x6c\x75\x73\x44\x4c\x43\x4d\x6f\x75\x6e\x74\x65\x72\x00\x76\x69\x73\x69\x62\x6c\x65\x49\x6e\x44\x4c\x43\x4d\x65\z
    \x6e\x75\x00\x42\x6f\x6f\x6c\x00\x64\x6c\x63\x5f\x74\x77\x33\x73\x65\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\z
    \x00\x00\xb2\xa3\xa8\xd4\x10\x00\x00\x00\xa0\x3e\xd0\xcd\x13\x00\x00\x00\xdb\x5b\x10\xf1\x19\x00\x00\x00\xe7\xbd\z
    \xd3\x97\x25\x00\x00\x00\x68\xde\xd4\xa7\x36\x00\x00\x00\x88\xe2\xce\xf4\x3d\x00\x00\x00\x41\x73\x86\xf3\x55\x00\z
    \x00\x00\xce\x0a\x16\x08\x5e\x00\x00\x00\x18\x7c\xf8\xaf\x80\x00\x00\x00\x89\x0d\x88\x5a\x99\x00\x00\x00\x58\xfa\z
    \xc7\xf7\xaf\x00\x00\x00\x06\xff\x47\x49\xd8\x00\x00\x00\x1c\x49\x8a\xd6\xf6\x00\x00\x00\x88\xb0\x23\x07\x14\x01\z
    \x00\x00\x6b\x1c\xd3\xe2\x2b\x01\x00\x00\x64\x5e\x76\x6b\x4a\x01\x00\x00\x84\xd8\x9e\x1c\x5b\x01\x00\x00\x67\xd6\z
    \xe1\x10\x60\x01\x00\x00\xdd\x56\xcc\x0c\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\z
    \x00\x20\x00\x00\x00\x00\x54\x00\x00\x00\xea\x02\x00\x00\x00\x00\x00\x00\x95\x60\xcd\xf1\x0a\x00\x08\x20\x01\x00\z
    \x00\x00\x3c\x00\x00\x00\x3e\x03\x00\x00\x00\x00\x00\x00\xc4\x55\xc1\xde\x00\x02\x00\x03\x00\x06\x00\x00\x00\x13\z
    \x00\x05\x00\x06\x00\x13\x00\x00\x00\x8e\x74\x77\x33\x73\x65\x5f\x64\x6c\x63\x5f\x6e\x61\x6d\x65\x07\x00\x06\x00\z
    \x13\x00\x00\x00\x8e\x74\x77\x33\x73\x65\x5f\x64\x6c\x63\x5f\x64\x65\x73\x63\x08\x00\x09\x00\x0c\x00\x00\x00\x01\z
    \x00\x00\x00\x02\x00\x00\x00\x11\x00\x12\x00\x05\x00\x00\x00\x00\x00\x00\x00\x0b\x00\x06\x00\x35\x00\x00\x00\xb0\z
    \x64\x6c\x63\x5c\x64\x6c\x63\x74\x77\x33\x73\x65\x5c\x64\x61\x74\x61\x5c\x67\x61\x6d\x65\x70\x6c\x61\x79\x5c\x69\z
    \x74\x65\x6d\x73\x5c\x74\x77\x33\x73\x65\x5f\x69\x74\x65\x6d\x73\x2e\x78\x6d\x6c\x00\x00"

)TW3SELIB" R"TW3SELIB(
  local function attr(k, v) return string.format(' %s="%s"', k, esc(v)) end
  local function write_defs()
    local mods = {}
    for fid in pairs(defs) do mods[#mods + 1] = fid end
    table.sort(mods)
    local d = {items = {}, abilities = {}}
    for _, fid in ipairs(mods) do
      for n, v in pairs(defs[fid].items) do d.items[n] = v end
      for n, v in pairs(defs[fid].abilities) do d.abilities[n] = v end
    end
    local out = {'<?xml version="1.0" encoding="UTF-8"?>',
                 '<!-- Written by TW3SE from the w3.items calls of ' .. esc(table.concat(mods, ", ")) .. '. It comes again at each start. -->',
                 '<redxml>', '<definitions>', '<abilities>'}
    local names = {}
    for n in pairs(d.abilities) do names[#names + 1] = n end
    table.sort(names)
    for _, n in ipairs(names) do
      local a = d.abilities[n]
      out[#out + 1] = '  <ability' .. attr("name", n) .. '>'
      local stats = {}
      for s in pairs(a.stats or {}) do stats[#stats + 1] = s end
      table.sort(stats)
      for _, s in ipairs(stats) do
        local v = a.stats[s]
        if type(v) ~= "table" then v = {min = v} end
        out[#out + 1] = '    <' .. s .. attr("type", v.type or "add") .. attr("min", v.min or 0) ..
                        (v.max and attr("max", v.max) or "") .. '/>'
      end
      for _, t in ipairs(a.tags or {}) do out[#out + 1] = '    <tags>' .. esc(t) .. '</tags>' end
      out[#out + 1] = '  </ability>'
    end
    out[#out + 1] = '</abilities>'
    out[#out + 1] = '<items>'
    names = {}
    for n in pairs(d.items) do names[#names + 1] = n end
    table.sort(names)
    for _, n in ipairs(names) do
      local it = d.items[n]
      local line = '  <item' .. attr("name", n) .. attr("category", it.category or "misc") .. attr("price", it.price or 1)
      line = line .. attr("grid_size", it.grid_size or 1) .. attr("ability_slots", it.ability_slots or 0)
      line = line .. attr("stackable", it.stackable or 1)
      if it.icon then line = line .. attr("icon_path", it.icon) end
      line = line .. attr("localisation_key_name", "tw3se_item_" .. n)
      line = line .. attr("localisation_key_description", "tw3se_item_desc_" .. n)
      local extra = {}
      for k in pairs(it.attrs or {}) do extra[#extra + 1] = k end
      table.sort(extra)
      for _, k in ipairs(extra) do line = line .. attr(k, it.attrs[k]) end
      out[#out + 1] = line .. '>'
      if it.tags and #it.tags > 0 then out[#out + 1] = '    <tags>' .. esc(table.concat(it.tags, ", ")) .. '</tags>' end
      if it.abilities and #it.abilities > 0 then
        out[#out + 1] = '    <base_abilities>'
        for _, a in ipairs(it.abilities) do out[#out + 1] = '      <a>' .. esc(a) .. '</a>' end
        out[#out + 1] = '    </base_abilities>'
      end
      out[#out + 1] = '  </item>'
    end
    out[#out + 1] = '</items>'
    out[#out + 1] = '</definitions>'
    out[#out + 1] = '</redxml>'
    out[#out + 1] = ''
    local file = ITEMS .. "\\tw3se_items.xml"
    local xml = table.concat(out, "\n")
    w3.fs.mkdir(ITEMS)
    if w3.fs.read(file) ~= xml then
      local ok, err = w3.fs.write(file, xml)
      if not ok then print("w3.items: " .. file .. ": " .. tostring(err)) end
    end
    local reddlc = DLC .. "\\dlctw3se.reddlc"
    if w3.fs.read(reddlc) ~= DLCTW3SE then w3.fs.write(reddlc, DLCTW3SE) end
    return file
  end
  local function file_id()
    local m = w3._mod():gsub("^mods/", ""):gsub("^autorun/", ""):gsub("%.lua$", "")
    if m == "" then m = "console" end
    local id = id_of(m)
    defs[id] = defs[id] or {items = {}, abilities = {}}
    return id
  end
  function w3.items.add(def)  -- an item of the mod's: name, category, price, stackable, icon, tags, abilities, text
    if type(def) ~= "table" or type(def.name) ~= "string" or def.name == "" then
      error("w3.items.add{name = ...}: a name is needed", 2)
    end
    local fid = file_id()
    defs[fid].items[def.name] = def
    w3._loctext("tw3se_item_" .. def.name, def.text or def.name)
    w3._loctext("tw3se_item_desc_" .. def.name, def.description or "")
    return write_defs()
  end
  function w3.items.ability(def)  -- an ability of the mod's: name, stats = {vitality = {type = "add", min = 100}}
    if type(def) ~= "table" or type(def.name) ~= "string" or def.name == "" then
      error("w3.items.ability{name = ...}: a name is needed", 2)
    end
    local fid = file_id()
    defs[fid].abilities[def.name] = def
    return write_defs()
  end
  w3._as("tw3se", w3.after, 3, function()
    if SAFE then return end
    for _, e in ipairs(w3.fs.list(ITEMS) or {}) do
      local old = not e.dir and e.name:match("^tw3se_.+%.xml$") and e.name ~= "tw3se_items.xml"
      if old or (not next(defs) and e.name == "tw3se_items.xml") then w3.fs.remove(ITEMS .. "\\" .. e.name) end
    end
    if not next(defs) and w3.fs.exists(DLC .. "\\dlctw3se.reddlc") then w3.fs.remove(DLC .. "\\dlctw3se.reddlc") end
  end)

)TW3SELIB" R"TW3SELIB(
  -- Key bindings of mods in the game's Options > Key Bindings (not yet measured in the game): an action is a
  -- line in the PCInput group of bin\config\r4game\user_config_matrix\pc\input.xml (the menu's entries; the game's
  -- igmOptions.ws asks for 'PCInput') and lines IK_<key>=(Action=<name>) in the context sections of
  -- Documents\The Witcher 3\input.settings (the bindings; the player's own changes stay, an action already bound
  -- in a section is left as it is). TW3SE writes its part of input.xml between markers, both files keep a copy
  -- (.tw3se_backup) before the first change; the game reads them at its start. The label goes through lockeys.
  local IK = {Space = "IK_Space", Enter = "IK_Enter", Tab = "IK_Tab", Backspace = "IK_Backspace", Home = "IK_Home",
              End = "IK_End", Insert = "IK_Insert", Delete = "IK_Delete", PageUp = "IK_PageUp", PageDown = "IK_PageDown",
              Mouse3 = "IK_MiddleMouse", Mouse4 = "IK_Mouse4", Mouse5 = "IK_Mouse5", Up = "IK_Up", Down = "IK_Down",
              Left = "IK_Left", Right = "IK_Right", Shift = "IK_LShift", Ctrl = "IK_LControl", Alt = "IK_Alt",
              Pad_A = "IK_Pad_A_CROSS", Pad_B = "IK_Pad_B_CIRCLE", Pad_X = "IK_Pad_X_SQUARE", Pad_Y = "IK_Pad_Y_TRIANGLE",
              Pad_LB = "IK_Pad_LeftShoulder", Pad_RB = "IK_Pad_RightShoulder", Pad_LT = "IK_Pad_LeftTrigger",
              Pad_RT = "IK_Pad_RightTrigger", Pad_LS = "IK_Pad_LeftThumb", Pad_RS = "IK_Pad_RightThumb",
              Pad_Back = "IK_Pad_Back_Select", Pad_Start = "IK_Pad_Start", Pad_Up = "IK_Pad_DigitUp",
              Pad_Down = "IK_Pad_DigitDown", Pad_Left = "IK_Pad_DigitLeft", Pad_Right = "IK_Pad_DigitRight"}
  local function ik(key)  -- "H" -> "IK_H", "F7" -> "IK_F7", "Num4" -> "IK_NumPad4", "Pad_Y" -> "IK_Pad_Y_TRIANGLE"
    if type(key) ~= "string" or key == "" then return nil end
    if IK[key] then return IK[key] end
    local k = key:upper()
    if k:match("^[A-Z]$") or k:match("^[0-9]$") or k:match("^F%d%d?$") then return "IK_" .. k end
    local n = key:match("^[Nn]um(%d)$")
    if n then return "IK_NumPad" .. n end
    return nil
  end
  local CONTEXTS = {"Exploration", "Exploration_Replacer_Ciri", "Combat", "Combat_Replacer_Ciri", "Horse",
                    "Horse_Replacer_Ciri", "Swimming", "Diving", "Boat"}
  local actions, flushing = {}, false  -- name -> {key, pad, label, contexts}
  w3.input = w3.input or {}
  local INPUT_XML = CONFIG .. "\\input.xml"
  local function settings_file() return w3.fs.folder("documents") .. "\\The Witcher 3\\input.settings" end
  local function backup(path, text)
    if text and not w3.fs.exists(path .. ".tw3se_backup") then w3.fs.write(path .. ".tw3se_backup", text) end
  end

  local function write_xml()
    local text = w3.fs.read(INPUT_XML)
    if not text then return end
    local nl = text:find("\r\n", 1, true) and "\r\n" or "\n"
    local B, E = "<!-- TW3SE begin", "<!-- TW3SE end -->"
    local a = text:find(B, 1, true)
    if a then  -- the old part goes, with its line
      local e = text:find(E, a, true)
      if not e then return end
      local ls = text:sub(1, a - 1):match(".*()" .. nl) or 1
      local le = text:find(nl, e, true)
      -- from the line break before the part up to the one after it: that one stays (with both gone, each start
      -- would join one more line of input.xml to the line before)
      text = text:sub(1, ls - 1) .. (le and text:sub(le) or "")
    end
    local names = {}
    for n in pairs(actions) do names[#names + 1] = n end
    table.sort(names)
    local part = {"\t\t\t" .. B .. ": the actions of Lua mods (w3.input.action), written again at each start -->"}
    for _, n in ipairs(names) do
      part[#part + 1] = string.format('\t\t\t<Var builder="Input" id="TW3SE_%s" displayName="tw3se_key_%s" displayType="INPUTPC" actions="%s"/>',
                                      esc(n), esc(n), esc(n))
    end
    part[#part + 1] = "\t\t\t" .. E
    if #names > 0 then
      local g = text:find('id="PCInput"', 1, true)
      local v = g and text:find("</VisibleVars>", g, true)
      if not v then return end
      local ls = text:sub(1, v - 1):match(".*()" .. nl) or v
      text = text:sub(1, ls - 1) .. nl .. table.concat(part, nl) .. text:sub(ls)
    end
    local old = w3.fs.read(INPUT_XML)
    if old ~= text then
      backup(INPUT_XML, old)
      w3.fs.write(INPUT_XML, text)
    end
  end

  local function write_settings()
    local path = settings_file()
    local text = w3.fs.read(path)
    if not text then return end
    local nl = text:find("\r\n", 1, true) and "\r\n" or "\n"
    local out, section, bound, changed = {}, nil, {}, false
    local function close_section()  -- the lines the open section is missing
      if not section then return end
      local names = {}
      for n in pairs(actions) do names[#names + 1] = n end
      table.sort(names)  -- the same file at every start
      for _, n in ipairs(names) do
        local act = actions[n]
        local want = false
        for _, c in ipairs(act.contexts) do if c == section then want = true end end
        if want and not bound[n] then
          for _, k in ipairs({ik(act.key), ik(act.pad)}) do
            if k then out[#out + 1] = k .. "=(Action=" .. n .. ")" changed = true end
          end
        end
      end
    end
    for line in (text .. nl):gmatch("(.-)" .. nl) do
      local s = line:match("^%[(.-)%]%s*$")
      if s then
        while #out > 0 and out[#out] == "" do table.remove(out) end
        close_section()
        out[#out + 1] = ""
        section, bound = s, {}
      else
        local act = line:match("%(Action=([%w_]+)")
        if act then bound[act] = true end
      end
      out[#out + 1] = line
    end
    while #out > 0 and out[#out] == "" do table.remove(out) end
    close_section()
    if changed then
      backup(path, text)
      local s = table.concat(out, nl):gsub("^" .. nl, "") .. nl
      w3.fs.write(path, s)
    end
  end

  local function flush()
    flushing = false
    local ok, err = pcall(write_xml)
    if not ok then print("w3.input.action: input.xml: " .. tostring(err)) end
    ok, err = pcall(write_settings)
    if not ok then print("w3.input.action: input.settings: " .. tostring(err)) end
  end

  -- w3.input.action{name, key, pad, label, contexts, on}: an action of the mod's in the game's key bindings;
  -- on(): runs when it is pressed (the game's own value of the action, from the next start on)
  function w3.input.action(def)
    if type(def) ~= "table" or type(def.name) ~= "string" or not def.name:match("^[%a][%w_]*$") then
      error("w3.input.action{name = ...}: a name of letters, digits and _", 2)
    end
    if def.key and not ik(def.key) then error("w3.input.action: no key " .. tostring(def.key) .. " (A-Z, 0-9, F1-F24, Num0-9, Space ...)", 2) end
    if def.pad and not ik(def.pad) then error("w3.input.action: no controller button " .. tostring(def.pad), 2) end
    actions[def.name] = {key = def.key, pad = def.pad, contexts = def.contexts or CONTEXTS}
    w3._loctext("tw3se_key_" .. def.name, def.label or def.name)
    if not flushing then
      flushing = true
      w3._as("tw3se", w3.after, 1, flush)
    end
    if def.on then
      -- The game knows the action from the start after input.xml and input.settings have it (it reads them before
      -- Lua runs). Until then the mod's own key and button count, in the action's contexts (the first start after
      -- installing).
      local was, known, checkAt = false, false, 0
      local inContext = {}
      for _, c in ipairs(def.contexts or CONTEXTS) do inContext[c] = true end
      local function game_knows(inp)
        local pc = inp:GetPCKeysForAction(def.name, {})
        local pad = inp:GetPadKeysForAction(def.name, {})
        return (type(pc) == "table" and #pc > 0) or (type(pad) == "table" and #pad > 0)
      end
      return w3.on_tick(function()
        local ok, v = pcall(function()
          local inp = w3.global("theInput")
          if not known and w3.time() >= checkAt then
            checkAt = w3.time() + 1
            local okk, k = pcall(game_knows, inp)
            known = okk and k
          end
          if known then return inp:GetActionValue(def.name) end
          if not inContext[tostring(inp:GetContext())] then return 0 end
          return ((def.key and w3.key_down(def.key)) or (def.pad and w3.key_down(def.pad))) and 1 or 0
        end)
        local down = ok and type(v) == "number" and v > 0.5
        if down and not was then def.on() end
        was = down
      end)
    end
  end

)TW3SELIB" R"TW3SELIB(
  -- 3 s after the start (the mods have loaded): a page whose mod is gone or has no such settings any more goes
  w3._as("tw3se", w3.after, 3, function()
    if SAFE then return end
    if not next(actions) then pcall(write_xml) end  -- no mod has an action any more: TW3SE's part of input.xml goes
    for _, e in ipairs(w3.fs.list(CONFIG) or {}) do
      local safe = not e.dir and e.name:match("^tw3se_(.+)%.[xX][mM][lL]$")
      if safe then
        local mine = false
        for name in pairs(pages) do if id_of(name) == safe then mine = true end end
        if not mine then w3.fs.remove(CONFIG .. "\\" .. e.name) end
      end
    end
  end)
end
)TW3SELIB"
R"TW3SELIB(
-- Messages between scripts, Lua and plugins (1.2): w3.send(channel, text) reaches every listener of the channel;
-- w3.on_message(channel, fn) runs fn(text, channel) for each (channel "*" for all), w3.off_message(h) stops it.
-- A script sends with TW3SE_Send(channel, text) and asks with TW3SE_Receive(channel); a plugin with SendMessage /
-- RegisterOnMessage. A sender gets its own messages too, so two directions use two channels ("mymod.cmd", "mymod.ui").
do
  local subs, nextH, started = {}, 0, false
  function w3.on_message(channel, fn)
    if type(fn) ~= "function" then error("w3.on_message(channel, fn): a function is needed", 2) end
    if not started then
      started = true
      w3._message_take()  -- the first call opens Lua's inbox
      w3._as("tw3se", w3.on_tick, function()
        for _, m in ipairs(w3._message_take()) do
          for _, s in pairs(subs) do
            if s.channel == "*" or s.channel == m[1] then w3._call_as(s.owner, s.fn, m[2], m[1]) end
          end
        end
      end)
    end
    nextH = nextH + 1
    subs[nextH] = {channel = channel or "*", fn = fn, owner = w3._mod()}
    return nextH
  end
  function w3.off_message(h) subs[h] = nil end
  function w3.send(channel, text)
    if type(channel) ~= "string" or channel == "" then error("w3.send(channel, text): a channel name is needed", 2) end
    w3._message_send(channel, text == nil and "" or tostring(text))
  end
end
)TW3SELIB"
R"TW3SELIB(
-- Texts without .w3strings (1.2): Mods\<mod>\tw3se\strings\<language>.txt (and tw3se\strings\) with lines
-- `key = text`. The game gets the text of its language for each key (GetLocStringByKeyExt: menu labels, option names,
-- item names), English when the language has none. "#" starts a comment line; "\n" in a text is a line break.
do
  local function parse(text, into, lang)
    for line in (text or ""):gmatch("[^\r\n]+") do
      if not line:match("^%s*#") then
        local key, value = line:match("^%s*([^=]-)%s*=%s*(.-)%s*$")
        if key and key ~= "" then
          into[key] = into[key] or {}
          into[key][lang] = (value:gsub("\\n", "\n"))
        end
      end
    end
  end
  local function load_dir(dir, into)
    for _, e in ipairs(w3.fs.list(dir) or {}) do
      local lang = not e.dir and e.name:match("^(%a%a%a?%a?)%.[tT][xX][tT]$")
      if lang then parse(w3.fs.read(dir .. "\\" .. e.name), into, lang:upper()) end
    end
  end
  function w3._load_strings()
    local keys = {}
    local game = w3.fs.folder("game")
    load_dir(w3.fs.folder("tw3se") .. "\\strings", keys)
    for _, m in ipairs(w3.fs.list(game .. "\\Mods") or {}) do
      if m.dir then load_dir(game .. "\\Mods\\" .. m.name .. "\\tw3se\\strings", keys) end
    end
    local n = 0
    for key, byLang in pairs(keys) do
      w3._loctext(key, byLang)
      n = n + 1
    end
    return n
  end
  -- once the game ticks: at the start the engine is not up (the language and the key table asked then leave the
  -- game's name pool locked, and the start hangs)
  w3._as("tw3se", w3.after, 1, function()
    local ok, n = pcall(w3._load_strings)
    if not ok then print("strings: " .. tostring(n)) elseif n > 0 then print("strings: " .. n .. " text key(s) from the mods' strings folders") end
  end)
end
)TW3SELIB"
R"TW3SELIB(
-- The game's Options > Mods (1.2): in patch 5.0 the list opens only its first 10 pages; from the 11th on a page stays
-- empty and the menu stops answering. TW3SE puts each page past the 10th in a submenu of its own with one entry,
-- "Settings", which opens the page (a first row of its list). It stays out with mods_menu_fix = off in tw3se.config
-- and when a mod fixes the list itself (Mod Settings Menu Fix, a paginator or organizer of the list).
do
  local ROWS = 10
  local SETTINGS = {EN = "Settings", DE = "Einstellungen", FR = "Paramètres", PL = "Ustawienia", RU = "Настройки",
                    ES = "Ajustes", IT = "Impostazioni", BR = "Configurações", CZ = "Nastavení", HU = "Beállítások",
                    TR = "Ayarlar", JP = "設定", KR = "설정", CN = "设置", ZH = "設定"}
  local function other_fix()
    if pcall(w3.call, "MSMF_Version") then return "Mod Settings Menu Fix" end
    local game = w3.fs.folder("game")
    for _, m in ipairs(w3.fs.list(game .. "\\Mods") or {}) do
      local n = m.name:lower()
      if m.dir and (n:find("paginat") or n:find("menuorganizer") or n:find("modsettingsmenufix")) then return m.name end
    end
  end
  -- a text the game has no translation for shows as "##option_free_alchemy": "Free alchemy" instead (mods' pages only)
  local function readable(key)
    local s = key:gsub("^##", ""):gsub("^preset_value_", ""):gsub("^preset_Mods_", ""):gsub("^option_", "")
    s = s:gsub("^panel_", ""):gsub("^tw3se_", ""):gsub("[_%.]+", " "):gsub("^%s+", ""):gsub("%s+$", "")
    return (s:gsub("^%l", string.upper))
  end
  local function relabel(arr, depth)
    if not arr or depth > 6 then return end
    for i = 0, arr:GetLength() - 1 do
      local o = arr:GetElementFlashObject(i)
      if o then
        local label = tostring(o:GetMemberFlashString("label") or "")
        if label:sub(1, 2) == "##" then o:SetMemberFlashString("label", readable(label))
        elseif label == "" then  -- a page whose name the game cannot find: its id, as in patch 4.04
          local id = tostring(o:GetMemberFlashString("id") or "")
          if id ~= "" then
            o:SetMemberFlashString("label", readable(id))
            o:SetMemberFlashString("listTitle", readable(id))
          end
        end
        local sub = o:GetMemberFlashArray("subElements")
        if sub then relabel(sub, depth + 1) end
      end
    end
  end
  local function fix(list, storage)
    local t = w3.enum("InGameMenuActionType")
    local holder, last = t.IGMActionType_MenuHolder, t.IGMActionType_MenuLastHolder
    local moved = 0
    for i = 0, list:GetLength() - 1 do
      local node = list:GetElementFlashObject(i)
      if node and tostring(node:GetMemberFlashString("id")):lower() == "mods" then
        local kids = node:GetMemberFlashArray("subElements")
        pcall(relabel, kids, 0)
        for k = ROWS, kids:GetLength() - 1 do
          local page = kids:GetElementFlashObject(k)
          if page and page:GetMemberFlashUInt("type") == last then
            local box = storage:CreateTempFlashObject()
            local label = page:GetMemberFlashString("label")
            box:SetMemberFlashString("id", tostring(page:GetMemberFlashString("id")) .. "_tw3se")
            box:SetMemberFlashString("label", label)
            box:SetMemberFlashString("listTitle", label)
            box:SetMemberFlashUInt("tag", page:GetMemberFlashUInt("tag"))
            box:SetMemberFlashUInt("type", holder)
            local sub = storage:CreateTempFlashArray()
            page:SetMemberFlashString("label", w3.tr(SETTINGS))
            sub:PushBackFlashObject(page)
            box:SetMemberFlashArray("subElements", sub)
            kids:SetElementFlashObject(k, box)
            moved = moved + 1
          end
        end
      end
    end
    return moved
  end
  w3._as("tw3se", w3.after, 1, function()
    if w3._tw3se_config("mods_menu_fix", "on") == "off" or w3._var_get("mod/TW3SE/mods_menu_fix") == "false" then
      return
    end
    local who = other_fix()
    if who then print("mods menu: " .. who .. " fixes the list - TW3SE stays out") return end
    local ok, err = pcall(w3._as, "tw3se", w3.hook, "IngameMenu_FillOptionsSubMenuData", function(self, args, orig)
      local r = orig()
      local okf, n = pcall(fix, r, args[1])
      if not okf then print("mods menu: " .. tostring(n))
      elseif n > 0 then w3._mods_menu_moved = n end
      return r
    end)
    if not ok then print("mods menu: not hooked: " .. tostring(err)) end
  end)
end
)TW3SELIB" R"TW3SELIB(
-- A gizmo in the world for a place and a heading (a scene's start, an object), dragged with the mouse while the
-- TW3SE Menu is open: the arrows move along X (red), Y (green) and Z (blue), the dot in the middle moves on the
-- ground plane, the ring turns the heading (the yellow line). Drawn with the game's own projection
-- (w3.view.to_screen), so it sits where the place is drawn.
do
  local gizmos = {}  -- id -> the drag that runs
  local function seg_dist(px, py, ax, ay, bx, by)
    local dx, dy = bx - ax, by - ay
    local l = dx * dx + dy * dy
    local t = l > 0 and math.max(0, math.min(1, ((px - ax) * dx + (py - ay) * dy) / l)) or 0
    local x, y = ax + dx * t - px, ay + dy * t - py
    return math.sqrt(x * x + y * y)
  end
  local function snap(v, step) if step and step > 0 then return math.floor(v / step + 0.5) * step end return v end
  local AXES = {{1, 0, 0, 0xE04040FF}, {0, 1, 0, 0x40D040FF}, {0, 0, 1, 0x4080FFFF}}
  local HOT, RING, DOT = 0xFFFFFFFF, 0xF0C020FF, 0xF0F0F0FF
  -- w3.ui.gizmo(id, pos, yaw [, opts]) -> changed, pos, yaw, active. opts: size (m; default 15 % of the distance to
  -- the camera), move = false, lift = false (no Z arrow), turn = false (no ring), snap (m), snap_angle (degrees),
  -- ground = true (the dot keeps it on the floor below it)
  function w3.ui.gizmo(id, pos, yaw, opts)
    opts = opts or {}
    local ui, d = w3.ui, w3.ui.draw
    local p = vec(pos)
    yaw = yaw or 0
    local cx, cy = w3.view.to_screen(p)
    local st = gizmos[id]
    if not cx then
      gizmos[id] = nil
      return false, p, yaw, false
    end
    local cam = w3.camera.pos()
    local len = opts.size or math.max(0.05, 0.15 * math.sqrt((cam.X - p.X) ^ 2 + (cam.Y - p.Y) ^ 2 + (cam.Z - p.Z) ^ 2))
    local function at(x, y, z) return w3.view.to_screen({X = p.X + x, Y = p.Y + y, Z = p.Z + z}) end
    local move, turn = opts.move ~= false, opts.turn ~= false
    -- the handles on the screen
    local ends = {}
    for k, a in ipairs(AXES) do
      if move and (k < 3 or opts.lift ~= false) then
        local ex, ey = at(a[1] * len, a[2] * len, a[3] * len)
        if ex then ends[k] = {ex, ey} end
      end
    end
    local ring = {}
    if turn then
      for i = 0, 48 do
        local t = i / 48 * 2 * math.pi
        local x, y = at(math.cos(t) * len * 0.8, math.sin(t) * len * 0.8, 0)
        if x then ring[#ring + 1] = x ring[#ring + 1] = y end
      end
    end
    local mx, my = ui.mouse()
    local hot = st and st.mode
    if not st and not ui.over_window() then
      local best = 8
      if move and math.sqrt((mx - cx) ^ 2 + (my - cy) ^ 2) < 9 then hot, best = "plane", 0 end
      for k, e in pairs(ends) do
        local dd = seg_dist(mx, my, cx, cy, e[1], e[2])
        if dd < best then hot, best = k, dd end
      end
      for i = 1, #ring - 3, 2 do
        local dd = seg_dist(mx, my, ring[i], ring[i + 1], ring[i + 2], ring[i + 3])
        if dd < best - 1 then hot, best = "ring", dd end
      end
      if hot and ui.mouse_clicked(1) then
        -- the screen's pixels per metre along X and Y at the start: the drag maps the mouse back through them
        local xx, xy = at(1, 0, 0)
        local yx, yy = at(0, 1, 0)
        st = {mode = hot, mx = mx, my = my, p = {X = p.X, Y = p.Y, Z = p.Z}, yaw = yaw,
              a = xx and {xx - cx, xy - cy}, b = yx and {yx - cx, yy - cy}}
        if type(hot) == "number" then
          local e = ends[hot]
          st.dir = {(e[1] - cx) / len, (e[2] - cy) / len}
        end
        if hot == "ring" and st.a and st.b then
          local u, v = nil, nil
          local det = st.a[1] * st.b[2] - st.b[1] * st.a[2]
          if math.abs(det) > 1e-6 then
            u = ((mx - cx) * st.b[2] - st.b[1] * (my - cy)) / det
            v = (st.a[1] * (my - cy) - (mx - cx) * st.a[2]) / det
            st.ang = math.deg(math.atan(v, u))
          end
        end
        gizmos[id] = st
      end
    end
    local changed = false
    if st then
      if not ui.mouse_down(1) then
        gizmos[id] = nil
        st = nil
      else
        local dx, dy = mx - st.mx, my - st.my
        local np, ny = {X = st.p.X, Y = st.p.Y, Z = st.p.Z}, st.yaw
        if type(st.mode) == "number" and st.dir then
          local l2 = st.dir[1] ^ 2 + st.dir[2] ^ 2
          if l2 > 1e-6 then
            local m = (dx * st.dir[1] + dy * st.dir[2]) / l2
            local a = AXES[st.mode]
            np.X, np.Y, np.Z = st.p.X + a[1] * m, st.p.Y + a[2] * m, st.p.Z + a[3] * m
          end
        elseif st.a and st.b then
          local det = st.a[1] * st.b[2] - st.b[1] * st.a[2]
          if math.abs(det) > 1e-6 then
            if st.mode == "plane" then
              np.X = st.p.X + (dx * st.b[2] - st.b[1] * dy) / det
              np.Y = st.p.Y + (st.a[1] * dy - dx * st.a[2]) / det
              if opts.ground then
                local f = floor_near(np.X, np.Y, st.p.Z, 3, nil, false)
                if f then np.Z = f.z end
              end
            elseif st.ang then
              local u = ((mx - cx) * st.b[2] - st.b[1] * (my - cy)) / det
              local v = (st.a[1] * (my - cy) - (mx - cx) * st.a[2]) / det
              ny = st.yaw + math.deg(math.atan(v, u)) - st.ang
            end
          end
        end
        if opts.snap then np.X, np.Y = snap(np.X, opts.snap), snap(np.Y, opts.snap) if st.mode ~= "plane" or not opts.ground then np.Z = snap(np.Z, opts.snap) end end
        if opts.snap_angle then ny = snap(ny, opts.snap_angle) end
        ny = (ny + 180) % 360 - 180
        changed = np.X ~= p.X or np.Y ~= p.Y or np.Z ~= p.Z or ny ~= yaw
        p, yaw = np, ny
      end
    end
    -- drawn behind the windows
    d.layer("back", function()
      if #ring >= 4 then d.poly(ring, hot == "ring" and HOT or RING, hot == "ring" and 3 or 2) end
      local r = math.rad(yaw)
      local hx, hy = w3.view.to_screen({X = p.X - math.sin(r) * len * 0.8, Y = p.Y + math.cos(r) * len * 0.8, Z = p.Z})
      local px, py = w3.view.to_screen(p)
      if px and hx and turn then d.line(px, py, hx, hy, RING, 3) d.disc(hx, hy, 4, RING) end
      for k, e in pairs(ends) do
        local c = hot == k and HOT or AXES[k][4]
        d.line(cx, cy, e[1], e[2], c, hot == k and 4 or 3)
        d.disc(e[1], e[2], 5, c)
      end
      if move then d.disc(cx, cy, 6, hot == "plane" and HOT or DOT) end
    end)
    return changed, p, yaw, st ~= nil
  end
end
)TW3SELIB"
