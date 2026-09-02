local pixy = require("pixy")

pixy.zone("prompt.left", {
  pixy.segment("directory", pixy.renderers.directory, {priority = 1}),
  pixy.segment("git", pixy.renderers.git, {priority = 2}),
  pixy.segment("status", pixy.renderers.status, {priority = 3}),
})

pixy.zone("activity", {
  pixy.segment("spinner", pixy.renderers.spinner),
})

pixy.zone("pokemon", {
  pixy.segment("sprite", pixy.renderers.pokemon),
})
