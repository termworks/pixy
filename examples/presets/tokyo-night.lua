-- Tokyo Night: rounded capsules in truecolour, one hue per kind of fact.
local pixy = require("pixy")
local git = require("pixy.segments.git")

local LEFT_CAP = utf8.char(0xE0B6)    -- rounded left cap
local RIGHT_CAP = utf8.char(0xE0B4)   -- rounded right cap

local ink = {26, 27, 38}
local hues = {
  directory = {122, 162, 247},
  git = {158, 206, 106},
  runtime = {224, 175, 104},
  failure = {247, 118, 142},
}

local function capsule(text, hue)
  return pixy.row({
    pixy.text(LEFT_CAP, {fg = hue}),
    pixy.text(text, {bg = hue, fg = ink, bold = true}),
    pixy.text(RIGHT_CAP, {fg = hue}),
  })
end

local function short_path(path)
  if not path then return "?" end
  local home = pixy.host.env("HOME")
  if home and path:sub(1, #home) == home then path = "~" .. path:sub(#home + 1) end
  local parts = {}
  for part in path:gmatch("[^/]+") do parts[#parts + 1] = part end
  if #parts <= 2 then return path end
  return table.concat({parts[#parts - 1], parts[#parts]}, "/")
end

pixy.zone("prompt.left", {
  pixy.segment("directory", function(ctx)
    return capsule(" " .. utf8.char(0xF07B) .. " " .. short_path(ctx.values.cwd) .. " ", hues.directory)
  end, {priority = 1}),
  pixy.segment("git", function(ctx)
    local branch = git.branch(ctx)
    if not branch then return nil end
    return capsule(" " .. utf8.char(0xE0A0) .. " " .. branch .. (git.status(ctx) == "dirty" and " *" or "") .. " ", hues.git)
  end, {priority = 2}),
  pixy.segment("language", function(ctx)
    if not ctx.values.language then return nil end
    return capsule(" " .. utf8.char(0xE7A8) .. " " .. ctx.values.language .. " ", hues.runtime)
  end, {priority = 4}),
  pixy.segment("status", function(ctx)
    if (tonumber(ctx.values.status) or 0) == 0 then return nil end
    return capsule(" " .. utf8.char(0xF0E7) .. " " .. ctx.values.status .. " ", hues.failure)
  end, {priority = 9}),
})
