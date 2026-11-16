-- Crowd Magnet: every nearby person goes limp (ragdoll), then is dragged toward the player a little every frame,
-- so limp bodies glide across the street and pile onto Geralt like iron filings on a magnet. Toggle with
-- crowd_magnet(true/false). A joke mod that shows the core of TW3SE: arbitrary logic, every frame, against every
-- live entity, through the game's own functions. (A real physics pull would use PhysX addForce, which is not wired
-- up yet; this slides them with a per-frame Teleport instead, which looks the same.)
local on = false
local caught = {}            -- entity address -> true (so each NPC is ragdolled only once)
local PULL = 0.18            -- fraction of the remaining distance covered each frame (0..1); higher = snappier

function crowd_magnet(enable, pull)
  on = enable ~= false
  if pull then PULL = pull end
  if not on then caught = {} end
  return on
end

w3.on_tick(function()
  if not on then return end
  local me = w3.player(); if not me then return end
  local pp = me:GetWorldPosition()
  local list = w3.call("FindGameplayEntitiesInRange", {}, me, 40.0, 40)
  if type(list) ~= "table" then return end
  for _, e in ipairs(list) do
    if e ~= me and tostring(w3.classof(e)):find("NPC") then
      local key = w3.addr(e)
      if not caught[key] then
        pcall(function() e:TurnOnRagdoll() end)   -- go limp, once
        caught[key] = true
      end
      local ok, np = pcall(function() return e:GetWorldPosition() end)
      if ok and np then                           -- slide a fraction of the way toward the player this frame
        local nx = np.X + (pp.X - np.X) * PULL
        local ny = np.Y + (pp.Y - np.Y) * PULL
        local nz = np.Z + (pp.Z + 0.5 - np.Z) * PULL
        pcall(function() e:Teleport({X = nx, Y = ny, Z = nz, W = 1}) end)
      end
    end
  end
end)
