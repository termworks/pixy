local nodes = require("pixy.nodes")
local layout = require("pixy.layout")
local encode = require("pixy.encode")

local M = {host = __pixy_host}

-- What `pixy.zone(name, ...)` registers into, and what the host reads off this
-- table once the config chunk has run. Kept in Lua so a config can read it back
-- or assign it outright.
M.zones = {}

for key, value in pairs(nodes) do
  if key ~= "item" then M[key] = value end
end
function M.sprite(options)
  return require("pixy.sprite").node(options)
end

local function regions(lines)
  local value = encode.regions(lines)
  if #value == 0 then return nil end
  return value
end

function M.config(config)
  if type(config) ~= "table" then error("pixy.config requires a table") end
  if type(config.zones) ~= "table" then error("pixy.config requires a zones table") end
  return config
end

function M.segment(name, render, options)
  if type(name) ~= "string" or name == "" then error("pixy.segment requires a name") end
  if type(render) ~= "function" then error("pixy.segment requires a render function") end
  if options ~= nil and type(options) ~= "table" then error("pixy.segment options must be a table") end
  return {kind = "pixy_segment", name = name, render = render, options = options or {}}
end

-- Registered by name, into a table the host reads after the chunk has run, so a
-- config is statements rather than one returned value. Registering a name twice
-- replaces it, which is what lets a config override a zone a preset set up.
--
-- `pixy.zone(segments)` is still a value constructor, for the configs that build
-- a `zones` table and return `pixy.config`.
function M.zone(name, segments)
  if segments == nil then
    if type(name) ~= "table" then error("pixy.zone requires a segment list") end
    return {kind = "pixy_zone", segments = name}
  end
  if type(name) ~= "string" or name == "" then error("pixy.zone requires a zone name") end
  if type(segments) ~= "table" then error("pixy.zone requires a segment list") end
  M.zones[name] = {kind = "pixy_zone", segments = segments}
end

local function priority(value)
  if type(value) == "table" and value.kind == "priority" then return value.priority, value.value end
  return 0, value
end

function M._render(config, request)
  local context = request.context or {}
  context.now_ms = request.now_ms or 0
  context.width = request.width
  context.height = request.height
  local entries = {}
  local function render_segment(zone_name, segment)
    local ok, value = pcall(segment.render, context)
    if not ok then error("segment " .. zone_name .. "." .. segment.name .. ": " .. tostring(value)) end
    return nodes.item(value, segment.options)
  end
  for _, selector in ipairs(request.select or {}) do
    local zone_name = selector
    local zone = config.zones[zone_name]
    local selected_segment
    if zone == nil then
      zone_name, selected_segment = selector:match("^(.*)%.([^.]+)$")
      zone = zone_name and config.zones[zone_name] or nil
    end
    if zone == nil then
      if not request.ignore_missing then error("unknown zone or segment " .. selector) end
    else
      local value
      if selected_segment then
        local segment = zone.segment_index[selected_segment]
        if segment == nil then
          if not request.ignore_missing then error("unknown segment " .. selector) end
        else
          value = render_segment(zone_name, segment)
        end
      else
        local children = {}
        for _, segment in ipairs(zone.segments) do
          children[#children + 1] = render_segment(zone_name, segment)
        end
        value = nodes.segments(children)
      end
      if value ~= nil then
        local weight, unwrapped = priority(value)
        entries[#entries + 1] = {name = selector, value = unwrapped, priority = weight}
      end
    end
  end
  local lines, next_frame = layout.compose(entries, context, request.width)
  if request.mode == "line" then
    local text = encode.line(lines, request.target)
    return {mode = "line", text = text, width = layout.measure(lines), next_frame_ms = next_frame, regions = regions(lines), _stream_rewind = "\r\27[K"}
  end
  if request.mode == "run" then
    return {mode = "run", runs = encode.runs(lines), width = layout.measure(lines), next_frame_ms = next_frame, regions = regions(lines), _stream_rewind = "\r\27[K"}
  end
  if request.mode == "surface" then
    local ansi, width, height, rewind, visible = encode.surface(lines, request.width, request.height)
    return {mode = "surface", ansi = ansi, width = width, height = height, next_frame_ms = next_frame, regions = regions(visible), _stream_rewind = rewind}
  end
  error("unknown render mode " .. tostring(request.mode))
end

return M
