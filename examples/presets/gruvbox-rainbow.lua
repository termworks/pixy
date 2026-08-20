-- Gruvbox Rainbow: every block a different hue, arrows between them, one
-- palette table at the top so recolouring the whole prompt is one edit.
local pixy = require("pixy")
local git = require("pixy.segments.git")

local ARROW = utf8.char(0xE0B0)   -- powerline right arrow
local ink = {40, 40, 40}
local palette = {
  host = {211, 134, 155},
  directory = {250, 189, 47},
  git = {184, 187, 38},
  runtime = {131, 165, 152},
  time = {142, 192, 124},
}
local order = {"host", "directory", "git", "runtime", "time"}

local function shows(name, ctx)
  if name == "git" then return git.branch(ctx) ~= nil end
  if name == "runtime" then return ctx.values.language ~= nil end
  return true
end

-- The arrow after a block is painted in that block's colour over the next
-- block's colour, which is what makes the seam read as one ribbon.
local function next_hue(after, ctx)
  local seen = false
  for _, name in ipairs(order) do
    if seen and shows(name, ctx) then return palette[name] end
    if name == after then seen = true end
  end
  return nil
end

local function block(name, text, ctx)
  return pixy.row({
    pixy.text(" " .. text .. " ", {bg = palette[name], fg = ink, bold = true}),
    pixy.text(ARROW, {fg = palette[name], bg = next_hue(name, ctx)}),
  })
end

return pixy.config({
  zones = {
    ["prompt.left"] = pixy.zone({
      pixy.segment("host", function(ctx)
        return block("host", utf8.char(0xF108) .. " " .. (ctx.values.hostname or "local"), ctx)
      end, {priority = 5}),
      pixy.segment("directory", function(ctx)
        return block("directory", utf8.char(0xF07B) .. " " .. ((ctx.values.cwd or "?"):match("([^/]+)/?$") or "/"), ctx)
      end, {priority = 1}),
      pixy.segment("git", function(ctx)
        local branch = git.branch(ctx)
        if not branch then return nil end
        return block("git", utf8.char(0xE0A0) .. " " .. branch, ctx)
      end, {priority = 2}),
      pixy.segment("runtime", function(ctx)
        if not ctx.values.language then return nil end
        return block("runtime", utf8.char(0xE7A8) .. " " .. ctx.values.language, ctx)
      end, {priority = 4}),
      pixy.segment("time", function(ctx)
        return block("time", utf8.char(0xF017) .. " " .. (ctx.values.time or "12:34"), ctx)
      end, {priority = 6}),
    }),
  },
})
