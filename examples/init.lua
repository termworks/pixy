local pixy = require("pixy")

-- The caller's cwd when it passed one, else the environment's.
local function cwd_of(ctx)
  return ctx.values.cwd or pixy.host.env("PWD")
end

pixy.zone("hello", {
  pixy.segment("label", function()
    return pixy.text(" hello ", {fg = 15, bg = 24, bold = true})
  end),
  pixy.segment("directory", function(ctx)
    return pixy.text(cwd_of(ctx) or "?", {fg = 14})
  end),
})
