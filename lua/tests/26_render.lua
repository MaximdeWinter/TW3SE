-- The renderer. Material parameters: a wraith spawned after a change shows the new lamp colour, the ones before
-- keep theirs.

local WRAITH = "characters\\npc_entities\\monsters\\wraith_lvl1.w2ent"

test("w3.material reads and changes a material's parameters", function()
  if not w3.player() then return end
  local e = w3.spawn(WRAITH)
  local ok, err = pcall(function()
    local mats = w3.material.of(e)
    check(#mats >= 1, "materials: " .. #mats)
    local lamp
    for _, m in ipairs(mats) do
      if m.class == "CMaterialInstance" and w3.material.get(m.material, "EmissivePower") then lamp = m.material end
    end
    check(lamp ~= nil, "the lantern's material")
    local power = w3.material.get(lamp, "EmissivePower")
    check(type(power) == "number", "a float: " .. tostring(power))
    w3.material.set(lamp, "EmissivePower", 5)
    eq(w3.material.get(lamp, "EmissivePower"), 5.0)
    local col = w3.material.get(lamp, "EmisiveColor")
    w3.material.set(lamp, "EmisiveColor", {Red = 1, Green = 2, Blue = 3, Alpha = 255})
    local c2 = w3.material.get(lamp, "EmisiveColor")
    eq(c2.Red .. " " .. c2.Green .. " " .. c2.Blue, "1 2 3")
    w3.material.set(lamp, "EmisiveColor", {Red = col.Red, Green = col.Green, Blue = col.Blue, Alpha = col.Alpha})
    w3.material.set(lamp, "EmissivePower", power)
    is_error(function() w3.material.set(lamp, "NoSuchParameter", 1) end, "no parameter")
    is_error(function() w3.material.set(e, "EmissivePower", 1) end, "CMaterialInstance")
    check(type(w3.material.apply(e)) == "number", "apply counts the meshes drawn anew")
    is_error(function() w3.refresh(e) end, "CDrawableComponent")
  end)
  w3.destroy(e)
  if not ok then error(err, 0) end
end)
