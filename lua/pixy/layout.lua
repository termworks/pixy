local style = require("pixy.style")
local host = __pixy_host
local M = {}

local function width(text)
  return host.cell_width(text)
end

local function valid_text(text)
  for index = 1, #text do
    local byte = string.byte(text, index)
    if byte < 32 or byte == 127 then error("text contains a control byte") end
  end
end

local function run(text, run_style, transparent, region)
  valid_text(text)
  return {
    text = text,
    style = style.merge({}, run_style),
    width = width(text),
    transparent = transparent or nil,
    region = region,
  }
end

local function flex(weight, region)
  local value = run("", {}, true, region)
  value.flex = math.max(0, tonumber(weight) or 1)
  return value
end

local function skip(cells, region)
  cells = math.max(0, math.floor(tonumber(cells) or 0))
  return run(string.rep(" ", cells), {}, true, region)
end

local function copy_runs(runs)
  local result = {}
  for _, value in ipairs(runs) do
    result[#result + 1] = {
      text = value.text,
      style = style.merge({}, value.style),
      width = value.width,
      transparent = value.transparent,
      flex = value.flex,
      region = value.region,
    }
  end
  return result
end

local function line_width(line)
  local result = 0
  for _, value in ipairs(line or {}) do result = result + value.width end
  return result
end

local function measure(lines)
  local maximum = 0
  for _, line in ipairs(lines) do maximum = math.max(maximum, line_width(line)) end
  return maximum
end

local function horizontal(parts)
  local height = 1
  for _, lines in ipairs(parts) do height = math.max(height, #lines) end
  local output = {}
  for row = 1, height do
    local line = {}
    for _, lines in ipairs(parts) do
      local block_width = measure(lines)
      local source = lines[row] or {}
      for _, value in ipairs(source) do line[#line + 1] = value end
      local missing = block_width - line_width(source)
      if missing > 0 then line[#line + 1] = skip(missing) end
    end
    output[row] = line
  end
  return output
end

-- The soonest of two cadences. Both are "ask again in N ms", so the smaller
-- number is the sooner one -- the same comparison an absolute deadline wanted,
-- which is why this needed no change when the meaning was pinned down.
local function earlier(left, right)
  if not left then return right end
  if not right then return left end
  return math.min(left, right)
end

local function child_priority(value)
  if type(value) == "table" and (value.kind == "priority" or value.kind == "item") then
    return tonumber(value.priority) or 0
  end
  return 0
end

local function child_list(value)
  if value == nil then return {} end
  if type(value) == "table" and value.kind ~= nil then return {value} end
  if type(value) == "table" then return value end
  return {value}
end

local function entry_width(entries)
  local result = 0
  for _, entry in ipairs(entries) do
    if entry.present then result = result + entry.width end
  end
  return result
end

local function entry_count(entries)
  local result = 0
  for _, entry in ipairs(entries) do
    if entry.present then result = result + 1 end
  end
  return result
end

local function removal_order(entries)
  local result = {}
  for _, entry in ipairs(entries) do result[#result + 1] = entry end
  table.sort(result, function(left, right)
    if left.priority == right.priority then return left.index > right.index end
    return left.priority > right.priority
  end)
  return result
end

local function entries_lines(entries)
  local parts, next_frame = {}, nil
  for _, entry in ipairs(entries) do
    if entry.present then
      parts[#parts + 1] = entry.lines
      next_frame = earlier(next_frame, entry.next_frame)
    end
  end
  return horizontal(parts), next_frame
end

local flatten

local function render_entries(children, ctx, inherited, index_base)
  local entries = {}
  for index, child in ipairs(child_list(children)) do
    local lines, next_frame = flatten(child, ctx, inherited)
    entries[#entries + 1] = {
      index = (index_base or 0) + index,
      priority = child_priority(child),
      lines = lines,
      width = measure(lines),
      next_frame = next_frame,
      present = true,
    }
  end
  return entries
end

local function flatten_segments(children, ctx, inherited, limit)
  local entries = render_entries(children, ctx, inherited)
  local total = entry_width(entries)
  if limit and total > limit then
    local present = entry_count(entries)
    for _, entry in ipairs(removal_order(entries)) do
      if total <= limit or present <= 1 then break end
      if entry.width > 0 then
        entry.present = false
        total = total - entry.width
        present = present - 1
      end
    end
  end
  return entries_lines(entries)
end

local function zone_geometry(left, center, right, limit)
  local left_width = entry_width(left)
  local center_width = entry_width(center)
  local right_width = entry_width(right)
  local center_x = math.max(0, math.floor((limit - center_width) / 2))
  local right_x = math.max(0, limit - right_width)
  local fits = left_width <= limit and center_width <= limit and right_width <= limit
  if center_width > 0 then
    fits = fits and left_width <= center_x
    if right_width > 0 then fits = fits and center_x + center_width <= right_x end
  elseif right_width > 0 then
    fits = fits and left_width <= right_x
  end
  return fits, left_width, center_width, right_width, center_x, right_x
end

local function flatten_regions(value, ctx, inherited)
  local limit = math.max(0, tonumber(ctx.width) or 0)
  local left = render_entries(value.left, ctx, inherited, 0)
  local center = render_entries(value.center, ctx, inherited, #left)
  local right = render_entries(value.right, ctx, inherited, #left + #center)
  local all = {}
  for _, entries in ipairs({left, center, right}) do
    for _, entry in ipairs(entries) do all[#all + 1] = entry end
  end
  local fits = zone_geometry(left, center, right, limit)
  local present = entry_count(all)
  if not fits then
    for _, entry in ipairs(removal_order(all)) do
      if fits or present <= 1 then break end
      entry.present = false
      present = present - 1
      fits = zone_geometry(left, center, right, limit)
    end
  end
  local _, left_width, center_width, right_width, center_x, right_x = zone_geometry(left, center, right, limit)
  local left_lines, left_next = entries_lines(left)
  local center_lines, center_next = entries_lines(center)
  local right_lines, right_next = entries_lines(right)
  local height = math.max(#left_lines, #center_lines, #right_lines)
  local output = {}
  local function append_at(line, cursor, x, source, block_width)
    if x > cursor then line[#line + 1] = skip(x - cursor) end
    for _, value_run in ipairs(source or {}) do line[#line + 1] = value_run end
    local used = line_width(source or {})
    if block_width > used then line[#line + 1] = skip(block_width - used) end
    return math.max(cursor, x + block_width)
  end
  for row = 1, height do
    local line, cursor = {}, 0
    if left_width > 0 then cursor = append_at(line, cursor, 0, left_lines[row], left_width) end
    if center_width > 0 then cursor = append_at(line, cursor, center_x, center_lines[row], center_width) end
    if right_width > 0 then append_at(line, cursor, right_x, right_lines[row], right_width) end
    output[row] = line
  end
  if #output == 0 then output[1] = {} end
  return output, earlier(earlier(left_next, center_next), right_next)
end

local function flatten_row(children, ctx, inherited)
  local parts = {}
  local next_frame
  for _, child in ipairs(children or {}) do
    local child_lines, child_next = flatten(child, ctx, inherited)
    parts[#parts + 1] = child_lines
    if child_next and (not next_frame or child_next < next_frame) then next_frame = child_next end
  end
  return horizontal(parts), next_frame
end

local function flatten_column(children, ctx, inherited)
  local lines = {}
  local next_frame
  for _, child in ipairs(children or {}) do
    local child_lines, child_next = flatten(child, ctx, inherited)
    if #child_lines > 0 then
      if #lines == 0 then
        for _, line in ipairs(child_lines) do lines[#lines + 1] = copy_runs(line) end
      else
        for _, line in ipairs(child_lines) do lines[#lines + 1] = copy_runs(line) end
      end
    end
    if child_next and (not next_frame or child_next < next_frame) then next_frame = child_next end
  end
  if #lines == 0 then lines[1] = {} end
  return lines, next_frame
end

local function interaction(value, ctx)
  local id = value.id
  if type(id) ~= "string" or not id:match("^[%w][%w_.-]*$") then
    error("region requires a valid id")
  end
  local actions = {}
  for name, action in pairs(value.actions or {}) do
    if type(name) ~= "string" or type(action) ~= "string" or name == "" or action == "" then
      error("region actions must map non-empty strings")
    end
    actions[name] = action
  end
  local hover_style = style.merge({}, value.hover_style or {})
  local press_styles = {}
  for name, pressed_style in pairs(value.press_styles or {}) do
    if type(name) ~= "string" or name == "" then error("region press style requires a button name") end
    press_styles[name] = style.merge({}, pressed_style)
  end
  local selected = {}
  local values = ctx.values or {}
  if values.hover_region == id then selected = style.merge(selected, hover_style) end
  if values.press_region == id and type(values.press_button) == "string" then
    selected = style.merge(selected, press_styles[values.press_button] or {})
  end
  return {
    id = id,
    actions = actions,
    hover_style = hover_style,
    press_styles = press_styles,
  }, selected
end

local function annotate(lines, metadata, selected_style)
  for _, line in ipairs(lines) do
    for _, value in ipairs(line) do
      value.region = metadata
      value.style = style.merge(value.style, selected_style)
    end
  end
  return lines
end

local function truncate_text(text, max_width)
  if max_width <= 0 then return "" end
  if width(text) <= max_width then return text end
  local result = ""
  for character in text:gmatch(utf8.charpattern) do
    local candidate = result .. character
    if width(candidate) > max_width then break end
    result = candidate
  end
  return result
end

local function truncate_lines(lines, max_width, marker)
  local result = {}
  for _, line in ipairs(lines) do
    local output, used = {}, 0
    local total = 0
    for _, value in ipairs(line) do total = total + value.width end
    local marker_width = total > max_width and width(marker) or 0
    local limit = math.max(0, max_width - marker_width)
    for _, value in ipairs(line) do
      if used >= limit then break end
      if value.transparent then
        local cells = math.min(value.width, limit - used)
        if cells > 0 then output[#output + 1] = skip(cells, value.region); used = used + cells end
      else
        local text = truncate_text(value.text, limit - used)
        if text ~= "" then
          output[#output + 1] = run(text, value.style, false, value.region)
          used = used + width(text)
        end
      end
    end
    if total > max_width and marker_width <= max_width then output[#output + 1] = run(marker, {}) end
    result[#result + 1] = output
  end
  return result
end

flatten = function(value, ctx, inherited)
  inherited = inherited or {}
  if value == nil or value == false then return {{} }, nil end
  if type(value) == "string" or type(value) == "number" then return {{run(tostring(value), inherited)}}, nil end
  if type(value) ~= "table" then error("segment returned unsupported " .. type(value)) end
  local kind = value.kind
  if kind == "text" then return {{run(value.text or "", style.merge(inherited, value.style))}}, value.next_frame_ms end
  if kind == "transparent" then return {{skip(value.width)}}, value.next_frame_ms end
  if kind == "spacer" then return {{flex(value.weight)}}, nil end
  if kind == "row" then return flatten_row(value.children, ctx, inherited) end
  if kind == "segments" then return flatten_segments(value.children, ctx, inherited, ctx.width) end
  if kind == "regions" then return flatten_regions(value, ctx, inherited) end
  if kind == "column" or kind == "surface" then return flatten_column(value.children or value.lines, ctx, inherited) end
  if kind == "pad" then
    local lines, next_frame = flatten(value.value, ctx, inherited)
    local padding = value.padding or {}
    for _, line in ipairs(lines) do
      if (padding.left or 0) > 0 then table.insert(line, 1, run(string.rep(" ", padding.left), inherited)) end
      if (padding.right or 0) > 0 then line[#line + 1] = run(string.rep(" ", padding.right), inherited) end
    end
    for _ = 1, math.max(0, padding.top or 0) do table.insert(lines, 1, {}) end
    for _ = 1, math.max(0, padding.bottom or 0) do lines[#lines + 1] = {} end
    return lines, next_frame
  end
  if kind == "priority" then return flatten(value.value, ctx, inherited) end
  if kind == "item" then
    local lines, next_frame = flatten(value.value, ctx, inherited)
    if value.id ~= nil then
      local metadata, selected_style = interaction(value, ctx)
      annotate(lines, metadata, selected_style)
    elseif value.actions ~= nil or value.hover_style ~= nil or value.press_styles ~= nil then
      error("interactive segment requires an id")
    end
    return lines, next_frame
  end
  if kind == "region" then
    local lines, next_frame = flatten(value.value, ctx, inherited)
    local metadata, selected_style = interaction(value, ctx)
    return annotate(lines, metadata, selected_style), next_frame
  end
  if kind == "style" then return flatten(value.value, ctx, style.merge(inherited, value.style)) end
  if kind == "truncate" then
    local lines, next_frame = flatten(value.value, ctx, inherited)
    return truncate_lines(lines, math.max(0, value.width or 0), value.marker or ""), next_frame
  end
  if kind == "spinner" then
    if value.spinner == "knight_rider" then
      local node, next_frame = require("pixy.animate").knight_rider(value, ctx)
      local lines, nested_next = flatten(node, ctx, inherited)
      if nested_next and nested_next < next_frame then next_frame = nested_next end
      return lines, next_frame
    end
    local frames = value.frames or {"-", "\\", "|", "/"}
    if #frames == 0 then return {{} }, nil end
    local interval = math.max(1, tonumber(value.interval_ms) or 80)
    local started = tonumber(value.started_at_ms or ctx.values.started_at_ms or 0) or 0
    local now = tonumber(ctx.now_ms or 0) or 0
    local elapsed = math.max(0, now - started)
    local index = math.floor(elapsed / interval) % #frames + 1
    local next_frame
    -- Relative: "ask again in N ms", which is what the protocol means by this.
    -- Sending an absolute timestamp made every host clamp it away as an enormous
    -- interval, so an animation silently ran at the refresh rate instead.
    if #frames > 1 then next_frame = interval - (elapsed % interval) end
    return {{run(tostring(frames[index]), style.merge(inherited, value.style))}}, next_frame
  end
  if kind == "animate" then
    if type(value.callback) ~= "function" then error("animate requires a function") end
    local lines, next_frame = flatten(value.callback(ctx), ctx, inherited)
    if value.interval_ms then
      local interval = math.max(1, tonumber(value.interval_ms) or 1)
      local started = tonumber(value.started_at_ms or ctx.values.started_at_ms or 0) or 0
      local now = tonumber(ctx.now_ms or 0) or 0
      local elapsed = math.max(0, now - started)
      local scheduled = interval - (elapsed % interval)
      if not next_frame or scheduled < next_frame then next_frame = scheduled end
    end
    return lines, next_frame
  end
  if kind == "sprite" then
    local sprite, sprite_next = require("pixy.sprite").resolve(value, ctx)
    local lines, next_frame = flatten(sprite, ctx, inherited)
    if sprite_next and (not next_frame or sprite_next < next_frame) then next_frame = sprite_next end
    return lines, next_frame
  end
  error("unknown node kind " .. tostring(kind))
end

function M.measure(lines)
  return measure(lines)
end

local function resolve_flex(lines, max_width)
  if max_width == nil or max_width < 0 then return lines end
  for _, line in ipairs(lines) do
    local content, total = 0, 0
    for _, value in ipairs(line) do
      content = content + value.width
      if value.flex then total = total + value.flex end
    end
    if total > 0 then
      local slack = math.max(0, max_width - content)
      local seen, given = 0, 0
      for _, value in ipairs(line) do
        if value.flex then
          seen = seen + value.flex
          local target = math.floor(slack * seen / total)
          value.text = string.rep(" ", target - given)
          value.width = target - given
          given = target
        end
      end
    end
  end
  return lines
end

function M.compose(entries, ctx, max_width)
  local rendered = {}
  for index, entry in ipairs(entries) do
    local ok, lines, next_frame = pcall(flatten, entry.value, ctx, {})
    if not ok then error("zone selection " .. tostring(entry.name) .. ": " .. tostring(lines)) end
    rendered[#rendered + 1] = {
      index = index,
      priority = tonumber(entry.priority) or 0,
      lines = lines,
      width = M.measure(lines),
      next_frame = next_frame,
      present = true,
    }
  end
  local total = 0
  for _, value in ipairs(rendered) do total = total + value.width end
  if max_width and max_width >= 0 and total > max_width then
    local removal = {}
    for _, value in ipairs(rendered) do removal[#removal + 1] = value end
    local present = #removal
    table.sort(removal, function(left, right)
      if left.priority == right.priority then return left.index > right.index end
      return left.priority > right.priority
    end)
    for _, value in ipairs(removal) do
      if total <= max_width or present <= 1 then break end
      if value.width > 0 then
        value.present = false
        total = total - value.width
        present = present - 1
      end
    end
  end
  local parts, next_frame = {}, nil
  for _, value in ipairs(rendered) do
    if value.present then
      parts[#parts + 1] = value.lines
      if value.next_frame and (not next_frame or value.next_frame < next_frame) then next_frame = value.next_frame end
    end
  end
  local lines = resolve_flex(horizontal(parts), max_width)
  if max_width and total > max_width then lines = M.clip(lines, max_width, #lines) end
  return lines, next_frame
end

function M.clip(lines, width_limit, height_limit)
  local output = {}
  for index = 1, math.min(#lines, math.max(0, height_limit or #lines)) do
    output[#output + 1] = truncate_lines({lines[index]}, math.max(0, width_limit or 0), "")[1]
  end
  return output
end

return M
