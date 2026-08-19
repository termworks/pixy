local style = require("pixy.style")
local layout = require("pixy.layout")
local M = {}

local function plain_line(line)
  local values = {}
  for _, run in ipairs(line) do values[#values + 1] = run.text end
  return table.concat(values)
end

local function ansi_line(line, preserve_transparency)
  local values, active, active_style = {}, "", {}
  for _, run in ipairs(line) do
    if preserve_transparency and run.transparent then
      if active ~= "" then values[#values + 1] = "\27[0m"; active = ""; active_style = {} end
      if run.width > 0 then values[#values + 1] = "\27[" .. tostring(run.width) .. "C" end
      goto continue
    end
    local descriptor = style.sgr(run.style)
    if descriptor ~= active then
      local sequence = style.transition(active_style, run.style)
      if sequence ~= "" then values[#values + 1] = sequence end
      active = descriptor
      active_style = run.style
    end
    values[#values + 1] = run.text
    ::continue::
  end
  if active ~= "" then values[#values + 1] = "\27[0m" end
  return table.concat(values)
end

local function shell_escape(text, target)
  if target == "bash" then
    return text:gsub("(\27%[[0-9;]*m)", "\\[%1\\]")
  end
  if target == "zsh" then
    return text:gsub("(\27%[[0-9;]*m)", "%%{%1%%}")
  end
  return text
end

function M.line(lines, target)
  if #lines > 1 then error("line output cannot contain multiple lines") end
  local line = lines[1] or {}
  if target == "plain" then return plain_line(line) end
  return shell_escape(ansi_line(line, false), target)
end

function M.runs(lines)
  if #lines > 1 then error("run output cannot contain multiple lines") end
  local output = {}
  for _, run in ipairs(lines[1] or {}) do
    local descriptor = style.describe(run.style)
    local previous = output[#output]
    if previous and previous.style == descriptor then
      previous.text = previous.text .. run.text
    else
      output[#output + 1] = {text = run.text, style = descriptor}
    end
  end
  return output
end

function M.regions(lines)
  local found, order = {}, {}
  for row, line in ipairs(lines) do
    local x = 0
    for _, run in ipairs(line) do
      local metadata = run.region
      if metadata and run.width > 0 then
        local region = found[metadata.id]
        if not region then
          local press_styles = {}
          for name, value in pairs(metadata.press_styles or {}) do
            press_styles[name] = style.describe(value)
          end
          region = {
            id = metadata.id,
            x = x,
            y = row - 1,
            width = run.width,
            height = 1,
            actions = metadata.actions or {},
            hover_style = style.describe(metadata.hover_style or {}),
            press_styles = press_styles,
          }
          found[metadata.id] = region
          order[#order + 1] = metadata.id
        else
          local right = math.max(region.x + region.width, x + run.width)
          local bottom = math.max(region.y + region.height, row)
          region.x = math.min(region.x, x)
          region.y = math.min(region.y, row - 1)
          region.width = right - region.x
          region.height = bottom - region.y
        end
      end
      x = x + run.width
    end
  end
  local output = {}
  for _, id in ipairs(order) do output[#output + 1] = found[id] end
  return output
end

function M.surface(lines, width, height)
  lines = layout.clip(lines, width, height)
  local output = {}
  for _, line in ipairs(lines) do output[#output + 1] = ansi_line(line, true) end
  local rewind = "\r"
  if #lines > 1 then rewind = "\27[" .. tostring(#lines - 1) .. "A\r" end
  return table.concat(output, "\n"), layout.measure(lines), #lines, rewind, lines
end

return M
