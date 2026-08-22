-- Powerline: solid blocks separated by arrow glyphs. Needs a Nerd Font.
local pixy = require("pixy")
local git = require("pixy.segments.git")

local ARROW = utf8.char(0xE0B0)   -- powerline right arrow

local function fish_path(path)
  if not path then return nil end
  local home = pixy.host.env("HOME")
  if home and path:sub(1, #home) == home then path = "~" .. path:sub(#home + 1) end
  local parts = {}
  for part in path:gmatch("[^/]+") do parts[#parts + 1] = part end
  for index = 1, #parts - 1 do parts[index] = parts[index]:sub(1, 1) end
  return (path:sub(1, 1) == "~" and "" or "/") .. table.concat(parts, "/")
end

-- Each block paints its own background and hands the next one a matching arrow,
-- so the seam is a glyph rather than a gap.
local function block(text, bg, fg, next_bg)
  return pixy.row({
    pixy.text(" " .. text .. " ", {bg = bg, fg = fg, bold = true}),
    pixy.text(ARROW, {fg = bg, bg = next_bg}),
  })
end

pixy.zone("prompt.left", {
  pixy.segment("directory", function(ctx)
    local branch = git.branch(ctx)
    return block(fish_path(ctx.values.cwd) or "?", 25, 255, branch and 240 or nil)
  end, {priority = 1}),
  pixy.segment("git", function(ctx)
    local branch = git.branch(ctx)
    if not branch then return nil end
    local mark = git.status(ctx) == "dirty" and " ●" or ""
    return block(utf8.char(0xE0A0) .. " " .. branch .. mark, 240, 255, nil)
  end, {priority = 2}),
  pixy.segment("status", function(ctx)
    if (ctx.values.status or 0) == 0 then return nil end
    return pixy.text(" " .. ctx.values.status .. " ", {bg = 160, fg = 255, bold = true})
  end, {priority = 9}),
})
