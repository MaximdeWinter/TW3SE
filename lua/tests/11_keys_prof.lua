-- Keys and the profiler: key names, on_key/off_key, w3.profile, hook handlers run as the mod that set them.

test("key names turn into key codes", function()
  local vk, mods = w3._key("F5")
  eq(vk, 0x74); eq(mods, 0)
  vk, mods = w3._key("Ctrl+Shift+K")
  eq(vk, string.byte("K")); eq(mods, 3)
  vk, mods = w3._key("alt+mouse4")
  eq(vk, 0x05); eq(mods, 4)
  eq((w3._key("Num7")), 0x67)
  eq((w3._key("0x70")), 0x70)
  eq((w3._key("7")), string.byte("7"))
end)

test("an unknown key name is an error", function()
  local ok, err = pcall(w3.key_down, "Hyper+F5")
  check(not ok and tostring(err):find("unknown key", 1, true), tostring(err))
  ok, err = pcall(w3.on_key, "NoSuchKey", function() end)
  check(not ok and tostring(err):find("unknown key", 1, true), tostring(err))
end)

test("key_down answers, on_key gives a handle that off_key takes back", function()
  eq(type(w3.key_down("Space")), "boolean")
  local h = w3.on_key("Ctrl+F12", function() end)
  eq(type(h), "number")
  w3.off_key(h)
end)

test("a hook handler runs as the mod that set it", function()
  local seen
  local id = w3._as("test/owner", function()
    return w3.hook("StrLen", function(self, args, orig) seen = w3._mod() return orig() end)
  end)
  w3.call("StrLen", "abc")
  w3.unhook(id)
  eq(seen, "test/owner")
end)

test("the profiler counts a hook's calls under its mod", function()
  local id = w3._as("test/prof", function()
    return w3.hook("StrLen", function(self, args, orig) return orig() end)
  end)
  for _ = 1, 5 do w3.call("StrLen", "witcher") end
  w3.unhook(id)
  local found
  for _, e in ipairs(w3.profile()) do
    if e.mod == "test/prof" and e.kind == "hook StrLen" then found = e end
  end
  check(found, "an entry for test/prof, hook StrLen")
  if found then
    check(found.calls >= 5, "5 calls counted: " .. found.calls)
    check(found.ms >= 0 and found.max >= 0, "times are not negative")
  end
end)
