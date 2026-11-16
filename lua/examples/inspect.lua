-- Press I to write the player's fields and values to tw3se\out\player_dump.txt for inspection.
w3.on_key("I", function()
  local p = w3.player()
  if p then
    w3.call("TW3SE_WriteNamed", "player_dump.txt", w3.dump(p))
    print("inspect: wrote player_dump.txt")
  end
end)
