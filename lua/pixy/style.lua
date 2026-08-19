local M = {}

local names = {
  black = 0, red = 1, green = 2, yellow = 3,
  blue = 4, magenta = 5, cyan = 6, white = 7,
  default = "default",
}

local function color(value)
  if value == nil then return nil end
  if type(value) == "string" then
    return names[value] or error("unknown color " .. value)
  end
  if type(value) == "number" then
    if value < 0 or value > 255 or value % 1 ~= 0 then error("palette color must be 0-255") end
    return value
  end
  if type(value) == "table" and #value == 3 then
    local rgb = {}
    for index = 1, 3 do
      local part = tonumber(value[index])
      if not part or part < 0 or part > 255 or part % 1 ~= 0 then error("RGB color must be 0-255") end
      rgb[index] = part
    end
    return rgb
  end
  error("invalid color")
end

function M.merge(base, overlay)
  local result = {}
  for key, value in pairs(base or {}) do result[key] = value end
  for key, value in pairs(overlay or {}) do result[key] = value end
  result.fg = color(result.fg)
  result.bg = color(result.bg)
  for _, key in ipairs({"bold", "dim", "italic", "underline", "reverse"}) do
    if result[key] ~= nil and type(result[key]) ~= "boolean" then error(key .. " style must be boolean") end
  end
  return result
end

local function color_name(value)
  if type(value) == "table" then return string.format("#%02x%02x%02x", value[1], value[2], value[3]) end
  return tostring(value)
end

function M.describe(style)
  style = M.merge({}, style)
  local fg, bg = style.fg, style.bg
  if style.reverse and fg ~= nil and fg ~= "default" and bg ~= nil and bg ~= "default" then
    fg, bg = bg, fg
  end
  local values = {}
  if fg ~= nil and fg ~= "default" then values[#values + 1] = "fg:" .. color_name(fg) end
  if bg ~= nil and bg ~= "default" then values[#values + 1] = "bg:" .. color_name(bg) end
  for _, key in ipairs({"bold", "dim", "italic", "underline"}) do
    if style[key] then values[#values + 1] = key end
  end
  return table.concat(values, " ")
end

local function append_color(codes, prefix, value)
  if value == nil then return end
  if value == "default" then
    codes[#codes + 1] = prefix == 38 and 39 or 49
  elseif type(value) == "table" then
    codes[#codes + 1] = prefix
    codes[#codes + 1] = 2
    for _, part in ipairs(value) do codes[#codes + 1] = part end
  else
    codes[#codes + 1] = prefix
    codes[#codes + 1] = 5
    codes[#codes + 1] = value
  end
end

local function same_color(left, right)
  if type(left) ~= type(right) then return false end
  if type(left) ~= "table" then return left == right end
  return left[1] == right[1] and left[2] == right[2] and left[3] == right[3]
end

function M.sgr(style)
  style = M.merge({}, style)
  local codes = {}
  append_color(codes, 38, style.fg)
  append_color(codes, 48, style.bg)
  if style.bold then codes[#codes + 1] = 1 end
  if style.dim then codes[#codes + 1] = 2 end
  if style.italic then codes[#codes + 1] = 3 end
  if style.underline then codes[#codes + 1] = 4 end
  if style.reverse then codes[#codes + 1] = 7 end
  if #codes == 0 then return "" end
  return "\27[" .. table.concat(codes, ";") .. "m"
end

function M.transition(current, next_style)
  current = M.merge({}, current)
  next_style = M.merge({}, next_style)
  local reset = (current.fg ~= nil and next_style.fg == nil) or (current.bg ~= nil and next_style.bg == nil)
  for _, key in ipairs({"bold", "dim", "italic", "underline", "reverse"}) do
    if current[key] and not next_style[key] then reset = true end
  end
  if reset then
    return "\27[0m" .. M.sgr(next_style)
  end
  local codes = {}
  if not same_color(current.fg, next_style.fg) then append_color(codes, 38, next_style.fg) end
  if not same_color(current.bg, next_style.bg) then append_color(codes, 48, next_style.bg) end
  local attributes = {bold = 1, dim = 2, italic = 3, underline = 4, reverse = 7}
  for _, key in ipairs({"bold", "dim", "italic", "underline", "reverse"}) do
    if next_style[key] and not current[key] then codes[#codes + 1] = attributes[key] end
  end
  if #codes == 0 then return "" end
  return "\27[" .. table.concat(codes, ";") .. "m"
end

return M
