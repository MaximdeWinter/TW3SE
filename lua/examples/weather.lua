-- Set the weather from the console: weather("WT_Rain"). The names come from GetWeatherConditionName and the
-- WT_* values; a blend time of 2 seconds, no quest pause.
function weather(name, blend)
  return w3.call("RequestWeatherChangeTo", name, blend or 2.0, false)
end
