-- A config that declares a palette, so the tests can pin what `palette set`
-- emits and which slot a `--palette` render claims.
local pixy = require("pixy")

return pixy.config({
  palette = {slot = 3, [15] = "#cdd6f4", bg = "#11111b"},
  zones = {
    ["prompt.left"] = pixy.zone({
      pixy.segment("hi", function()
        return pixy.text(" hi ", {fg = 15})
      end),
    }),
  },
})
