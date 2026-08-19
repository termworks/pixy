local M = {}

local function copy_style(value)
  local result = {}
  for key, item in pairs(value) do
    if type(item) == "table" then
      result[key] = {item[1], item[2], item[3]}
    else
      result[key] = item
    end
  end
  return result
end

local function parameter_list(raw)
  if raw == "" then return {0} end
  local result = {}
  for value in (raw .. ";"):gmatch("(.-);") do
    if value ~= "" and not value:match("^%d+$") then error("invalid SGR parameter") end
    result[#result + 1] = value == "" and 0 or tonumber(value)
  end
  return result
end

local function byte_color(values, index)
  local value = values[index]
  if value == nil or value < 0 or value > 255 then error("invalid SGR color") end
  return value
end

local function apply_sgr(current, raw)
  local values, index = parameter_list(raw), 1
  while index <= #values do
    local code = values[index]
    if code == 0 then
      current = {}
    elseif code == 1 then
      current.bold = true
    elseif code == 2 then
      current.dim = true
    elseif code == 3 then
      current.italic = true
    elseif code == 4 then
      current.underline = true
    elseif code == 7 then
      current.reverse = true
    elseif code == 22 then
      current.bold, current.dim = false, false
    elseif code == 23 then
      current.italic = false
    elseif code == 24 then
      current.underline = false
    elseif code == 27 then
      current.reverse = false
    elseif code >= 30 and code <= 37 then
      current.fg = code - 30
    elseif code >= 40 and code <= 47 then
      current.bg = code - 40
    elseif code >= 90 and code <= 97 then
      current.fg = code - 90 + 8
    elseif code >= 100 and code <= 107 then
      current.bg = code - 100 + 8
    elseif code == 39 then
      current.fg = "default"
    elseif code == 49 then
      current.bg = "default"
    elseif code == 38 or code == 48 then
      local field = code == 38 and "fg" or "bg"
      local mode = values[index + 1]
      if mode == 5 then
        current[field] = byte_color(values, index + 2)
        index = index + 2
      elseif mode == 2 then
        current[field] = {
          byte_color(values, index + 2),
          byte_color(values, index + 3),
          byte_color(values, index + 4),
        }
        index = index + 4
      else
        error("unsupported SGR color mode")
      end
    end
    index = index + 1
  end
  return current
end

function M.parse(text)
  local lines, line, style, start, index = {}, {}, {}, 1, 1
  local function flush(stop)
    if stop >= start then
      line[#line + 1] = {text = text:sub(start, stop), style = copy_style(style)}
    end
  end
  while index <= #text do
    local byte = text:byte(index)
    if byte == 27 then
      flush(index - 1)
      if text:sub(index + 1, index + 1) ~= "[" then error("sprite contains a non-SGR escape") end
      local stop = text:find("m", index + 2, true)
      if not stop or stop - index > 128 then error("sprite contains an invalid SGR escape") end
      style = apply_sgr(style, text:sub(index + 2, stop - 1))
      index, start = stop + 1, stop + 1
    elseif byte == 10 then
      flush(index - 1)
      lines[#lines + 1], line = line, {}
      index, start = index + 1, index + 1
    elseif byte == 13 and text:byte(index + 1) == 10 then
      flush(index - 1)
      lines[#lines + 1], line = line, {}
      index, start = index + 2, index + 2
    elseif byte < 32 or byte == 127 then
      error("sprite contains a control byte")
    else
      index = index + 1
    end
  end
  flush(#text)
  lines[#lines + 1] = line
  if #lines > 1 and #lines[#lines] == 0 then table.remove(lines) end
  return lines
end

return M
