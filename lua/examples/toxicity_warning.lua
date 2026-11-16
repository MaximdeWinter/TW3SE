-- Toxicity warning: after a potion, a message when the toxicity is above the warning level.
local cfg = w3.mod{
  name = "Toxicity Warning", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = {EN = "A warning when the toxicity gets high.", DE = "Eine Warnung, wenn die Toxizität hoch wird."},
  settings = {
    {key = "level", type = "slider", min = 30, max = 100, int = true, default = 75,
     label = {EN = "Warning at (percent)", DE = "Warnung ab (Prozent)"}},
  },
}

w3.on("consume", function()
  w3.after(0.5, function()
    local tox, max = w3.stats.get("toxicity")
    if max > 0 and tox / max * 100 >= cfg.level then
      w3.hud.message(w3.tr({EN = "Toxicity ", DE = "Toxizität "}) .. math.floor(tox / max * 100) .. " %", 3)
    end
  end)
end)
