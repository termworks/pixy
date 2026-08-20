local pixy = require("pixy")

-- Measures every codepoint the caller lists, so two builds can be compared
-- character by character rather than by whether a bar happens to line up.
return pixy.config({zones = {w = pixy.zone({pixy.segment("v", function(ctx)
  local total = {}
  for _, code in ipairs(ctx.values.codes or {}) do
    total[#total + 1] = tostring(pixy.host.cell_width(utf8.char(code)))
  end
  return table.concat(total, ",")
end)})}})
