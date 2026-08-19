local pixy = require("pixy.nodes")
local git = require("pixy.segments.git")
local M = {}

local function basename(path)
  if path == nil or path == "" then return nil end
  return tostring(path):match("([^/]+)/*$") or "/"
end

function M.prompt_left(ctx)
  local values = {
    pixy.text(" " .. (basename(ctx.values.cwd) or "?") .. " ", {fg = 15, bg = 24, bold = true}),
  }
  if (ctx.values.status or 0) ~= 0 then
    values[#values + 1] = pixy.text(" " .. tostring(ctx.values.status) .. " ", {fg = 15, bg = 1})
  end
  local branch = git.branch(ctx)
  if branch then values[#values + 1] = pixy.text(" " .. branch .. " ", {fg = 0, bg = 6}) end
  values[#values + 1] = pixy.text(" ", {})
  values[#values + 1] = M.character(ctx)
  return pixy.row(values)
end

function M.prompt_right(ctx)
  local values = {}
  if ctx.values.language then values[#values + 1] = pixy.text(" " .. ctx.values.language .. " ", {fg = 14}) end
  if (ctx.values.jobs or 0) > 0 then values[#values + 1] = pixy.text(" jobs:" .. tostring(ctx.values.jobs) .. " ", {fg = 11}) end
  if ctx.values.duration_ms and ctx.values.duration_ms > 0 then values[#values + 1] = pixy.text(" " .. tostring(ctx.values.duration_ms) .. "ms ", {fg = 8}) end
  if ctx.values.vimode then values[#values + 1] = pixy.text(" " .. ctx.values.vimode .. " ", {fg = 13}) end
  return pixy.row(values)
end

function M.directory(ctx) return basename(ctx.values.cwd) end
function M.status(ctx) return ctx.values.status end
function M.duration(ctx) return ctx.values.duration_ms end
function M.jobs(ctx) return ctx.values.jobs end
function M.last_command(ctx) return ctx.values and ctx.values.last_command or nil end

function M.character(ctx)
  if ctx.values and ctx.values.sudo then return pixy.text(" # ", {fg = 9, bold = true}) end
  local color = (ctx.values.status or 0) == 0 and 10 or 9
  return pixy.text(" ❯ ", {fg = color, bold = true})
end

function M.running(ctx)
  if not (ctx.values and ctx.values.running) then return nil end
  return pixy.spinner({frames = {"-", "\\", "|", "/"}, interval_ms = 80, started_at_ms = ctx.values.started_at_ms})
end

return M
