-- A marker and class label over every nearby actor, anchored in the world. The projection uses the game's own
-- GetBaseScreenPosition (the function the HUD uses to place icons); our overlay draws the markers. This renders in
-- retail, unlike the engine's CVisualDebug debug drawing (stripped from release).
--
-- The heavy part (find nearby actors + project each) runs on a 10 Hz timer and caches the result; the per-frame
-- draw just replays the cache, so it stays cheap. Nearby actors come from the native FindGameplayEntitiesInRange
-- (an in-engine spatial query). Engine screen coordinates are bottom-left origin and normalized, so y is flipped;
-- off-screen points (outside 0..1) are dropped. Colours are 0xRRGGBBAA (alpha last).
local markers = {}

w3.every(0.1, function()
  local me = w3.player(); if not me then markers = {}; return end
  local sw, sh = w3.screen_size()
  local list = w3.call("FindGameplayEntitiesInRange", {}, me, 40.0, 30)
  local out = {}
  if type(list) == "table" then
    for _, e in ipairs(list) do
      local cls = (e ~= me) and w3.classof(e) or nil
      if cls and (cls:find("NPC") or cls:find("Actor")) then          -- only actors, skip clutter
        local onscreen, sp = w3.call("GetBaseScreenPosition", {}, e, nil, 1.9, false, true)
        if onscreen and sp and sp.X >= 0 and sp.X <= 1 and sp.Y >= 0 and sp.Y <= 1 then
          out[#out + 1] = {x = sp.X * sw, y = (1 - sp.Y) * sh, label = cls}
        end
      end
    end
  end
  markers = out
end)

w3.on_draw(function()
  for _, m in ipairs(markers) do
    w3.draw.rect(m.x - 6, m.y - 6, 12, 12, 0xff3030ff)                       -- solid red box (alpha last)
    w3.draw.rect(m.x + 10, m.y - 10, #m.label * 9 + 8, 20, 0x000000cc)       -- dark label background
    w3.draw.text(m.x + 14, m.y - 8, 0xffffffff, m.label)
  end
end)
