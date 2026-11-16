-- Sign refund: each sign has a chance to give its stamina back, Quen and Igni their own chance.
local cfg = w3.mod{
  name = "Sign Refund", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = "A chance that a sign gives its stamina back.",
  settings = {
    {key = "chance", type = "slider", min = 0, max = 100, int = true, default = 20, label = "Chance (percent)"},
    {key = "quen", type = "slider", min = 0, max = 100, int = true, default = 10, label = "Quen (percent)"},
  },
}

w3.on("sign", function(e)
  local chance = e.sign == "ST_Quen" and cfg.quen or cfg.chance
  if math.random(100) > chance then return end
  w3.after(0.3, function()  -- after the game took the stamina
    local _, max = w3.stats.get("stamina")
    w3.stats.set("stamina", max)
    w3.hud.message("Stamina back", 1)
  end)
end)
