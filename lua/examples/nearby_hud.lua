-- Shows how many actors are within 30 m of the player, updated live on the HUD.
w3.on_draw(function()
  if not w3.player() then return end
  local n = #w3.near(30, "CActor")
  w3.draw.rect(20, 90, 220, 28, 0x80202020)
  w3.draw.text(32, 96, 0xffffffff, "Actors within 30 m: " .. n)
end)
