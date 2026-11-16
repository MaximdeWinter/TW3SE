-- Double the player's maximum health by wrapping the game's own GetMaxHealth. GetMaxHealth is a WitcherScript
-- method (not a native), so this is only possible with a script hook: the hook runs the original, doubles what it
-- returned, and only for the player (every other actor keeps its real value). Call double_health(false) to undo it.
local handle

function double_health(enable)
  if enable == false then
    if handle then w3.unhook(handle); handle = nil end
    return false
  end
  if not handle then
    handle = w3.hook("CActor", "GetMaxHealth", function(self, args, orig)
      local base = orig()                         -- run the original method
      if self == w3.player() then return base * 2 end  -- double it, but only for the player
      return base                                 -- every other actor is unchanged
    end)
  end
  return true
end

double_health(true)  -- on when this mod loads
