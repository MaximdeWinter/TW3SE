-- More field types: TagList as a live array of names, LocalizedString as its text, empty engine pointers as nil.

test("an entity's tags are a list of names", function()
  local p = w3.player(); if not p then return end
  local tags = p.tags
  check(#tags > 0, "the player has tags")
  local found = false
  for i = 1, #tags do if tags[i] == "PLAYER" then found = true end end
  check(found, "PLAYER is one of them")
end)

test("a tag can be added and removed", function()
  local p = w3.player(); if not p then return end
  local tags = p.tags
  local n = #tags
  tags:insert("tw3se_test_tag")
  eq(#p.tags, n + 1)
  eq(p.tags[n + 1], "tw3se_test_tag")
  eq(tags:remove(n + 1), "tw3se_test_tag")
  eq(#p.tags, n)
end)

test("a localized string reads as its text", function()
  local p = w3.player(); if not p then return end
  local name = p.displayName
  check(type(name) == "string" and #name > 0 and name:sub(1, 1) ~= "#", "displayName is a text: " .. tostring(name))
end)
