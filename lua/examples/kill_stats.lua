-- Kill stats: what the player killed, by name, in this save and in all games. A tab of its own in the TW3SE Menu.
local saved, store = w3.saved(), w3.store()
saved.kills, store.kills = saved.kills or {}, store.kills or {}

w3.on("load", function() saved.kills = saved.kills or {} end)

w3.on("death", function(e)
  if e.killer ~= w3.player() then return end
  local ok, name = pcall(function() return e.victim:GetDisplayName() end)
  name = ok and name ~= "" and name or w3.classof(e.victim)
  saved.kills[name] = (saved.kills[name] or 0) + 1
  store.kills[name] = (store.kills[name] or 0) + 1
end)

local function sorted(t)
  local list = {}
  for name, n in pairs(t) do list[#list + 1] = {name = name, n = n} end
  table.sort(list, function(a, b) return a.n > b.n end)
  return list
end

w3.mod{
  name = "Kill Stats", version = "1.0", author = "TW3SE", tw3se = "1.1.0",
  description = "What you killed, in this save and in all games.",
  tab = function()
    w3.ui.table("kills", {"Enemy", "This save", "All games"}, function()
      for _, k in ipairs(sorted(store.kills)) do
        w3.ui.row()
        w3.ui.col() w3.ui.text(k.name)
        w3.ui.col() w3.ui.text(tostring(saved.kills[k.name] or 0))
        w3.ui.col() w3.ui.text(tostring(k.n))
      end
    end)
  end,
}
