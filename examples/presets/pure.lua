-- Pure: no backgrounds, one accent colour, the prompt mark carries the state.
local pixy = require("pixy")
local git = require("pixy.segments.git")

local function short_path(path)
  if not path then return "?" end
  local home = pixy.host.env("HOME")
  if home and path:sub(1, #home) == home then return "~" .. path:sub(#home + 1) end
  return path
end

pixy.zone("prompt.left", {
  pixy.segment("directory", function(ctx)
    return pixy.text(short_path(ctx.values.cwd), {fg = 39, bold = true})
  end, {priority = 1}),
  pixy.segment("git", function(ctx)
    local branch = git.branch(ctx)
    if not branch then return nil end
    local mark = git.status(ctx) == "dirty" and "*" or ""
    return pixy.text(" " .. branch .. mark, {fg = 244})
  end, {priority = 2}),
  pixy.segment("duration", function(ctx)
    local ms = tonumber(ctx.values.duration_ms) or 0
    if ms < 2000 then return nil end
    return pixy.text(string.format(" %.1fs", ms / 1000), {fg = 179})
  end, {priority = 3}),
  -- The mark is the only thing that reports failure: magenta became red.
  pixy.segment("mark", function(ctx)
    local failed = (tonumber(ctx.values.status) or 0) ~= 0
    return pixy.text(" ❯ ", {fg = failed and 203 or 141, bold = true})
  end, {priority = 99}),
})
