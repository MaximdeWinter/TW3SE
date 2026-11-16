-- Add 10000 crowns once when the game starts.
local p = w3.player()
if p then
  p:AddMoney(10000)
  print("rich: added 10000 crowns, now " .. p:GetMoney())
end
