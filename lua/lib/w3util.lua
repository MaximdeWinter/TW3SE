-- Convenience helpers over the raw API. Use: local u = require("lib.w3util")
-- Each one is a thin wrapper around the game's own functions, so it does exactly what the game would.
local u = {}

function u.player()
  return w3.player()
end

-- Health -----------------------------------------------------------------------------------------------------
function u.heal()
  local p = w3.player()
  if p then p:SetHealth(p:GetMaxHealth()) end
end

function u.health()
  local p = w3.player()
  return p and p:GetHealth() or 0, p and p:GetMaxHealth() or 0
end

-- Money ------------------------------------------------------------------------------------------------------
function u.money()
  local p = w3.player()
  return p and p:GetMoney() or 0
end

function u.give_money(amount)
  local p = w3.player()
  if p then p:AddMoney(amount) end
end

-- Weather ----------------------------------------------------------------------------------------------------
function u.weather(name, blend)
  return w3.call("RequestWeatherChangeTo", name, blend or 2.0, false)
end

function u.current_weather()
  return w3.call("GetWeatherConditionName")
end

-- Time -------------------------------------------------------------------------------------------------------
-- Set the time of day to an hour (0..23). GameTimeCreate builds the GameTime, SetGameTime applies it.
function u.set_hour(hour)
  local g = w3.global("theGame")
  if g then g:SetGameTime(w3.call("GameTimeCreate", 0, hour, 0, 0), true) end
end

-- Entities ---------------------------------------------------------------------------------------------------
-- The first entity with this tag, or nil. For all of them as a list use w3.by_tag(tag).
function u.by_tag(tag)
  local g = w3.global("theGame")
  return g and g:GetEntityByTag(tag) or nil
end

-- Movement ---------------------------------------------------------------------------------------------------
-- Teleport the player to x, y, z. Caution: this moves Geralt and triggers streaming.
function u.teleport(x, y, z)
  local p = w3.player()
  if p then p:Teleport({X = x, Y = y, Z = z, W = 1}) end
end

function u.position()
  local p = w3.player()
  if not p then return end
  local v = p:GetWorldPosition()
  return v.X, v.Y, v.Z
end

-- Physics ----------------------------------------------------------------------------------------------------
-- Cast a ray through the world from `from` to `to` (each a {X, Y, Z} table or a Vector). Returns hit (bool),
-- the hit position and the hit normal. `groups` is a list of collision group names (default: static + terrain).
-- Built on CWorld:StaticTrace, which is native, so this is a real physics query, no hook needed.
function u.raycast(from, to, groups)
  local g = w3.global("theGame")
  local w = g and g.activeWorld
  if not w then return false end
  return w:StaticTrace(from, to, {}, {}, groups or {"Static", "Terrain", "Door", "Destructible"})
end

-- The ground height under a point: cast straight down and return the z where the world is hit, or nil.
function u.ground_z(x, y, z)
  local hit, pos = u.raycast({X = x, Y = y, Z = z + 50, W = 1}, {X = x, Y = y, Z = z - 1000, W = 1})
  if hit and pos then return pos.Z end
  return nil
end

-- Subsystems (all reflected, reachable without any hook) ------------------------------------------------------
function u.environment()
  local g = w3.global("theGame")
  return g and g.envMgr or nil  -- W3EnvironmentManager
end

function u.input()
  return w3.global("theInput")  -- CInputManager
end

function u.camera_director()
  local w = w3.global("theGame") and w3.global("theGame").activeWorld
  return w and w:GetCameraDirector() or nil  -- CR4CameraDirector
end

-- Facts (the quest/world state key-value store)
function u.fact(name)
  return w3.call("FactsQuerySum", name)
end

-- Nearby -----------------------------------------------------------------------------------------------------
-- Entities within `radius` of the player (optionally of a class). Thin wrapper over w3.near.
function u.near(radius, className)
  return w3.near(radius, className)
end

-- The single nearest entity of a class within `radius`, or nil.
function u.nearest(className, radius)
  local p = w3.player(); if not p then return nil end
  local pp = p:GetWorldPosition()
  local best, bestd = nil, (radius or 1e9)
  for _, e in ipairs(w3.entities(className)) do
    local ok, d = pcall(function() return w3.call("VecDistance", pp, e:GetWorldPosition()) end)
    if ok and d < bestd then best, bestd = e, d end
  end
  return best, bestd
end

-- Spawning ---------------------------------------------------------------------------------------------------
-- Spawn an entity at x,y,z (optional yaw). `template` is either a CEntityTemplate object (e.g. another entity's
-- .template) or a depot path string ("...\\x.w2ent"). Returns the new CEntity, or nil. PM_DontPersist: not saved.
-- Not the player's template: a spawned player entity makes its own CPlayerInput, which takes over every input
-- listener; once it is gone, Sprint, Esc and attacks do nothing until the save is loaded again.
function u.spawn(template, x, y, z, yaw)
  local g = w3.global("theGame")
  if not g then return nil end
  local tmpl = template
  if type(template) == "string" then tmpl = w3.call("LoadResource", template, true) end
  if not tmpl then return nil end
  local where = {X = x, Y = y, Z = z, W = 1.0}
  local rot = {Pitch = 0.0, Yaw = yaw or 0.0, Roll = 0.0}
  return g:CreateEntity(tmpl, where, rot, true, false, false, "PM_DontPersist", {})
end

-- Inventory --------------------------------------------------------------------------------------------------
function u.inventory()
  local p = w3.player()
  return p and p:GetInventory() or nil  -- CInventoryComponent
end

function u.has_item(name)
  local inv = u.inventory()
  return inv and inv:HasItem(name) or false
end

-- Adds `quantity` of an item by id (e.g. "Swallow", "Steel sword _crafted"). Returns the new unique ids.
function u.give_item(name, quantity)
  local inv = u.inventory()
  if inv then return inv:AddAnItem(name, quantity or 1, true, true, false, false) end
end

function u.remove_item(name, quantity)
  local inv = u.inventory()
  return inv and inv:RemoveItemByName(name, quantity or 1) or false
end

return u
