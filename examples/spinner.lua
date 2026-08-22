local pixy = require("pixy")

-- An animated node reports the deadline of its next distinct frame, so a
-- caller polls exactly when the picture changes and never on a timer.
pixy.zone("work", {
  pixy.segment("spinner", function()
    return pixy.spinner({
      kind = "knight_rider",
      width = 12,
      step = 40,
      hold = 20,
      colors = {117, 75, 68, 61, 60, 59, 238, 237},
      bg = 0,
      prefix = pixy.text(" ", {bg = 0}),
      suffix = pixy.text(" ", {bg = 0}),
    })
  end),
  pixy.segment("label", function()
    return pixy.text(" building ", {bg = 0, fg = 250})
  end),
})
