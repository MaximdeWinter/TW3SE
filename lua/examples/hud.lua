-- A small HUD: draw the player's health and money in the top-left corner every frame.
w3.on_draw(function()
  local p = w3.player()
  if not p then return end
  w3.draw.rect(20, 20, 240, 56, 0x80202020)
  w3.draw.text(32, 28, 0xffffffff, "Health " .. math.floor(p:GetHealth()) .. " / " .. math.floor(p:GetMaxHealth()))
  w3.draw.text(32, 48, 0xffd080ff, "Crowns " .. p:GetMoney())
end)
