-- The game's HUD, Gwent, the journal, map pins and facts. Every test puts back what it
-- changed.

test("w3.hud lists modules, turns one off and on, reads the HUD's visibility", function()
  if not w3.player() then return end
  local mods = w3.hud.modules()
  check(#mods > 10, "HUD modules: " .. #mods)
  local was = w3.hud.enabled("BuffsModule")
  eq(w3.hud.enabled("BuffsModule", not was), not was)
  eq(w3.hud.enabled("BuffsModule", was), was)
  check(type(w3.hud.visible()) == "boolean", "visibility")
  is_error(function() w3.hud.enabled("NoSuchModule") end, "no HUD module")
  w3.hud.message("TW3SE test", 1)
end)

test("w3.gwent reads the collection and the cards, adds a card and takes it back", function()
  if not w3.player() then return end
  local cards = w3.gwent.cards()
  check(#cards > 100, "card definitions: " .. #cards)
  check(type(cards[1].name) == "string" and cards[1].index >= 0, "a card with name and index")
  check(#w3.gwent.collection() > 0, "the collection")
  local card = cards[1].name
  local n = w3.gwent.count(card)
  eq(w3.gwent.add(card), n + 1)
  check(w3.gwent.has(card), "owned now")
  eq(w3.gwent.remove(card), n)
  check(not pcall(w3.gwent.add, card, 1e15), "1e15 copies refused")
  local deck = w3.gwent.deck()
  check(type(deck) == "string" and deck:find("^GwintFaction_") ~= nil, "the selected deck: " .. tostring(deck))
  eq(w3.gwent.unlocked(deck), true)
end)

test("w3.quest lists the journal and the tracked quest", function()
  if not w3.player() then return end
  local all = w3.quest.list()
  check(type(all) == "table", "the quests")
  for _, q in ipairs(all) do check(type(q.title) == "string" and q.status:find("^JS_") ~= nil, "title and status") end
  local active = w3.quest.list("JS_Active")
  check(#active <= #all, "a filtered list")
  local title, entry = w3.quest.tracked()
  if entry then eq(w3.quest.status(entry):sub(1, 3), "JS_") end
end)

-- With a pin limit of 0 every pin is refused; toggle_pin sets the limits the map menu sets (through an out
-- parameter that carries its value in).
test("w3.quest puts a map pin of the player's own and takes it away", function()
  if not w3.player() then return end
  local n = #w3.quest.pins()
  local ok, added = w3.quest.toggle_pin()
  check(ok, "the pin accepted")
  check(added ~= 0, "a pin added")
  eq(#w3.quest.pins(), n + 1)
  local ok2, _, removed = w3.quest.toggle_pin()
  check(ok2 and removed == added, "the same pin removed")
  eq(#w3.quest.pins(), n)
end)

test("w3.fact adds, sets, reads and removes facts", function()
  if not w3.player() then return end
  w3.fact.remove("tw3se_test_fact")
  eq(w3.fact.exists("tw3se_test_fact"), false)
  eq(w3.fact.add("tw3se_test_fact", 2), 2)
  eq(w3.fact.add("tw3se_test_fact", 3), 5)
  eq(w3.fact.set("tw3se_test_fact", 1), 1)
  w3.fact.remove("tw3se_test_fact")
  eq(w3.fact.get("tw3se_test_fact"), 0)
end)

test("w3.text.set gives a string id the game does not have a text, and takes it back", function()
  local id = 2112000123
  eq(w3.call("GetLocStringById", id), "")
  w3.text.set(id, "Hello from TW3SE")
  eq(w3.call("GetLocStringById", id), "Hello from TW3SE")
  w3.text.set(id, nil)
  eq(w3.call("GetLocStringById", id), "")
end)
