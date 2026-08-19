local nodes = require("pixy.nodes")
local ansi = require("pixy.ansi")
local style = require("pixy.style")
local M = {}
local asset_cache, asset_order = {}, {}

local function anchor_axes(anchor)
  local vertical, horizontal = "top", "left"
  local first, second = anchor:match("^([^-]+)%-([^-]+)$")
  local verticals = {top = true, center = true, bottom = true}
  local horizontals = {left = true, center = true, right = true}
  if first then
    if not verticals[first] or not horizontals[second] then error("invalid sprite anchor " .. anchor) end
    return first, second
  end
  if anchor == "center" then return "center", "center" end
  if verticals[anchor] then vertical = anchor
  elseif horizontals[anchor] then horizontal = anchor
  else error("invalid sprite anchor " .. anchor) end
  return vertical, horizontal
end

local function clip_left(text, cells)
  if cells <= 0 then return text end
  local target = math.max(0, __pixy_host.cell_width(text) - cells)
  local offset = 1
  for character in text:gmatch(utf8.charpattern) do
    offset = offset + #character
    local candidate = text:sub(offset)
    local first = candidate:match(utf8.charpattern)
    local starts_cluster = not first or __pixy_host.cell_width(first) > 0
    if starts_cluster and utf8.codepoint(character) ~= 0x200d and __pixy_host.cell_width(candidate) <= target then
      return candidate
    end
  end
  return ""
end

local function cached_asset(pack, name)
  local key = pack .. "\0" .. name
  if asset_cache[key] ~= nil then return asset_cache[key] end
  local data = __pixy_host.asset(pack, name)
  if data == nil then return nil end
  if #asset_order >= 8 then
    asset_cache[table.remove(asset_order, 1)] = nil
  end
  asset_cache[key], asset_order[#asset_order + 1] = data, key
  return data
end

local function split_text(values, text, run_style, transparent)
  if transparent == false then
    if text ~= "" then values[#values + 1] = nodes.text(text, run_style) end
    return
  end
  local start = 1
  while start <= #text do
    local space = text:find(" ", start, true)
    if not space then
      values[#values + 1] = nodes.text(text:sub(start), run_style)
      break
    end
    if space > start then values[#values + 1] = nodes.text(text:sub(start, space - 1), run_style) end
    local stop = space
    while text:sub(stop + 1, stop + 1) == " " do stop = stop + 1 end
    values[#values + 1] = nodes.transparent(stop - space + 1)
    start = stop + 1
  end
end

local function line_width(chunks)
  local result = 0
  for _, chunk in ipairs(chunks) do result = result + __pixy_host.cell_width(chunk.text) end
  return result
end

local function clip_chunks(chunks, cells)
  if cells <= 0 then return chunks end
  local result, remaining = {}, cells
  for _, chunk in ipairs(chunks) do
    local chunk_width = __pixy_host.cell_width(chunk.text)
    if chunk_width <= remaining then
      remaining = remaining - chunk_width
    else
      local text = clip_left(chunk.text, remaining)
      remaining = 0
      if text ~= "" then result[#result + 1] = {text = text, style = chunk.style} end
    end
  end
  return result
end

local function sprite_line(chunks, frame_width, left, base_style, transparent)
  chunks = clip_chunks(chunks, math.max(0, -left))
  local values = {}
  if left > 0 then values[#values + 1] = nodes.transparent(left) end
  for _, chunk in ipairs(chunks) do
    split_text(values, chunk.text, style.merge(base_style or {}, chunk.style), transparent)
  end
  local missing = frame_width - line_width(chunks) - math.max(0, -left)
  if missing > 0 and transparent == false then
    values[#values + 1] = nodes.text(string.rep(" ", missing), base_style)
  elseif missing > 0 then
    values[#values + 1] = nodes.transparent(missing)
  end
  return nodes.row(values)
end

local function split_lines(frame)
  local lines, start = {}, 1
  while true do
    local stop = frame:find("\n", start, true)
    if not stop then
      lines[#lines + 1] = frame:sub(start)
      break
    end
    local line = frame:sub(start, stop - 1)
    if line:sub(-1) == "\r" then line = line:sub(1, -2) end
    lines[#lines + 1] = line
    start = stop + 1
  end
  if #lines > 1 and lines[#lines] == "" then table.remove(lines) end
  return lines
end

local function frame_lines(frame, format)
  if format == "ansi" then return ansi.parse(frame) end
  if format ~= "plain" then error("invalid sprite format " .. tostring(format)) end
  local lines = {}
  for _, line in ipairs(split_lines(frame)) do lines[#lines + 1] = {{text = line, style = {}}} end
  return lines
end

local function position(options, ctx, frame_width, frame_height)
  if options.position then
    local selected = tostring(options.position):gsub("%-", "")
    local width, height = ctx.width or frame_width, ctx.height or frame_height
    if selected == "topleft" then return 1, 1 end
    if selected == "topright" then return width > frame_width + 2 and width - frame_width - 2 or 0, 1 end
    if selected == "bottomleft" then return 1, height > frame_height + 2 and height - frame_height - 2 or 0 end
    if selected == "bottomright" then
      local left = width > frame_width + 2 and width - frame_width - 2 or 0
      local top = height > frame_height + 2 and height - frame_height - 2 or 0
      return left, top
    end
    if selected == "center" then
      return width > frame_width and math.floor((width - frame_width) / 2) or 0,
        height > frame_height and math.floor((height - frame_height) / 2) or 0
    end
    error("invalid sprite position " .. tostring(options.position))
  end
  local vertical, horizontal = anchor_axes(options.anchor or "top-left")
  local left = 0
  if horizontal == "right" then
    left = (ctx.width or frame_width) - frame_width
  elseif horizontal == "center" then
    left = math.floor(((ctx.width or frame_width) - frame_width) / 2)
  end
  local top = 0
  if vertical == "bottom" then
    top = (ctx.height or frame_height) - frame_height
  elseif vertical == "center" then
    top = math.floor(((ctx.height or frame_height) - frame_height) / 2)
  end
  return left, top
end

local function select_frames(options)
  if options.frames then
    if type(options.frames) == "table" then return options.frames end
    return {options.frames}
  end
  if options.pack and options.name then
    local data = cached_asset(options.pack, options.name)
    if not data and options.fallback_name then data = cached_asset(options.pack, options.fallback_name) end
    if data then return {data} end
  end
  return nil
end

function M.node(options)
  options = options or {}
  options.kind = "sprite"
  return options
end

function M.resolve(options, ctx)
  local frames = select_frames(options)
  if frames and #frames > 0 then
    local interval = math.max(1, tonumber(options.interval_ms) or 100)
    local started = tonumber(options.started_at_ms or ctx.values.started_at_ms or 0) or 0
    local now = tonumber(ctx.now_ms or 0) or 0
    local elapsed = math.max(0, now - started)
    local index = math.floor(elapsed / interval) % #frames + 1
    local next_frame
    if #frames > 1 then next_frame = now + interval - (elapsed % interval) end
    local raw = tostring(frames[index])
    local format = options.format or (raw:find("\27", 1, true) and "ansi" or "plain")
    local raw_lines, frame_width = frame_lines(raw, format), 0
    for _, line in ipairs(raw_lines) do frame_width = math.max(frame_width, line_width(line)) end
    local left, top = position(options, ctx, frame_width, #raw_lines)
    left = left + (tonumber(options.x or options.offset_x or 0) or 0)
    top = top + (tonumber(options.y or options.offset_y or 0) or 0)
    if top < 0 then
      for _ = 1, math.min(#raw_lines, -top) do table.remove(raw_lines, 1) end
      top = 0
    end
    local lines = {}
    for _ = 1, top do lines[#lines + 1] = nodes.text("") end
    for _, line in ipairs(raw_lines) do
      lines[#lines + 1] = sprite_line(line, frame_width, left, options.style, options.transparent)
    end
    return nodes.column(lines), next_frame
  end
  return nil
end

return M
