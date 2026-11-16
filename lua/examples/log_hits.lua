-- Log every hit the player reacts to, without changing the game at all. ReactToBeingHit is a WitcherScript method;
-- this is an observe-only wrap: run the original, peek at its result, pass it straight through. A hook that returns
-- the original's own result (and nothing else) leaves behaviour exactly as it was.
local handle = w3.hook("W3PlayerWitcher", "ReactToBeingHit", function(self, args, orig)
  local reacted = orig()                       -- run the original reaction
  print("player hit (reacted = " .. tostring(reacted) .. ")")
  return reacted                               -- pass the original result through unchanged
end)

-- w3.unhook(handle) stops the logging.
