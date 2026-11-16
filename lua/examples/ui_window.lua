-- A window of your own while the TW3SE Menu is open (F2): the player's money and health, a button that adds 100
-- crowns and a slider for the time of day. Copy into lua\autorun\ to have it at every start.
local u = require("lib.w3util")
local hour = 12

w3.on_ui(function()
  w3.ui.window("Example", function()
    local p = w3.player()
    if not p then w3.ui.text("no player (main menu)") return end
    w3.ui.text("Crowns: " .. p:GetMoney())
    w3.ui.text(string.format("Health: %.0f", p:GetHealth()))
    if w3.ui.button("+100 crowns") then u.give_money(100) end
    local changed, h = w3.ui.slider("Hour", hour, 0, 23)
    if changed then
      hour = math.floor(h)
      u.set_hour(hour)
    end
  end, 360, 200)
end)
