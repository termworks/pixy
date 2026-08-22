local pixy = require("pixy")

-- Two segments and a priority. The status badge only exists when the last
-- command failed, and it is the one that survives when the width runs out.
pixy.zone("prompt.left", {
  pixy.segment("directory", function(ctx)
    return pixy.text(" " .. (ctx.values.cwd or "?") .. " ", {bg = 237, fg = 250})
  end, {priority = 1}),
  pixy.segment("status", function(ctx)
    if (ctx.values.status or 0) == 0 then return nil end
    return pixy.text(" " .. ctx.values.status .. " ", {bg = 1, fg = 15, bold = true})
  end, {priority = 5}),
})
