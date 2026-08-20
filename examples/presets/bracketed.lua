-- Bracketed: plain text only. No backgrounds, no glyphs, safe over ssh into
-- anything, and readable when the terminal has eight colours.
local pixy = require("pixy")
local git = require("pixy.segments.git")

local function basename(path)
  if not path then return "?" end
  return path:match("([^/]+)/?$") or path
end

local function wrap(text, style)
  return pixy.row({
    pixy.text("[", {fg = 8}),
    pixy.text(text, style),
    pixy.text("]", {fg = 8}),
  })
end

return pixy.config({
  zones = {
    ["prompt.left"] = pixy.zone({
      pixy.segment("directory", function(ctx)
        return wrap(basename(ctx.values.cwd), {fg = 4, bold = true})
      end, {priority = 1}),
      pixy.segment("git", function(ctx)
        local branch = git.branch(ctx)
        if not branch then return nil end
        return wrap(branch .. (git.status(ctx) == "dirty" and " *" or ""), {fg = 2})
      end, {priority = 2}),
      pixy.segment("jobs", function(ctx)
        local jobs = tonumber(ctx.values.jobs) or 0
        if jobs == 0 then return nil end
        return wrap(jobs .. " jobs", {fg = 6})
      end, {priority = 4}),
      pixy.segment("status", function(ctx)
        local status = tonumber(ctx.values.status) or 0
        if status == 0 then return nil end
        return wrap("exit " .. status, {fg = 1, bold = true})
      end, {priority = 9}),
      pixy.segment("mark", function() return pixy.text(" $ ", {fg = 7}) end, {priority = 99}),
    }),
  },
})
