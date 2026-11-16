-- w3.engine_hook runs a Lua function before an engine function of the address list, with its register
-- arguments. w3.refresh(component) calls CDrawableComponent::RefreshRenderProxies with the component in rcx.

local function a_component()
  local p = w3.player(); if not p then return end
  for _, c in pairs(w3.deform.meshes(p)) do return c end
end

test("w3.engine_hook sees an engine function's arguments, w3.engine_unhook takes it off", function()
  local c = a_component(); if not c then return end
  local seen, calls = nil, 0
  local id = w3.engine_hook("CDrawableComponent::RefreshRenderProxies", function(a)
    calls = calls + 1
    seen = a[1]
    check(a.name == "CDrawableComponent::RefreshRenderProxies", tostring(a.name))
  end)
  check(type(id) == "number", "an id")
  w3.refresh(c)
  eq(seen, w3.addr(c))
  local found
  for _, h in ipairs(w3.engine_hooks()) do
    if h.name == "CDrawableComponent::RefreshRenderProxies" then found = h end
  end
  check(found and found.calls >= 1 and found.handlers == 1, "listed with its calls")
  eq(w3.engine_unhook(id), true)
  w3.refresh(a_component())                                -- the refreshed component has a new handle
  eq(calls, 1)
  eq(w3.engine_unhook(id), false)
end)

test("an engine hook that errors leaves the function to run", function()
  local c = a_component(); if not c then return end
  local id = w3.engine_hook("CDrawableComponent::RefreshRenderProxies", function() error("on purpose") end)
  w3.refresh(c)                                            -- logged, the game's function ran
  w3.engine_unhook(id)
  is_error(function() w3.engine_hook("No::SuchFunction_TW3SE", function() end) end, "not in TW3SE's address list")
end)
