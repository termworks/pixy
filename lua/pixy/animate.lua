local M = {}

function M.frames(frames, interval_ms, now_ms, started_at_ms)
  if #frames == 0 then return nil, nil end
  local interval = math.max(1, tonumber(interval_ms) or 80)
  local elapsed = math.max(0, (tonumber(now_ms) or 0) - (tonumber(started_at_ms) or 0))
  local index = math.floor(elapsed / interval) % #frames + 1
  local next_frame
  if #frames > 1 then next_frame = (tonumber(now_ms) or 0) + interval - (elapsed % interval) end
  return frames[index], next_frame
end

local function knight_state(frame, width, hold)
  local forward_end = width
  local hold_end = forward_end + hold
  local backward_end = hold_end + width - 1
  if frame < forward_end then return frame, true, false, 0 end
  if frame < hold_end then return width - 1, true, true, frame - forward_end end
  if frame < backward_end then return width - 2 - (frame - hold_end), false, false, 0 end
  return 0, false, true, frame - backward_end
end

local function trail_index(cell, position, forward, holding, hold_progress, trail_length)
  local distance = forward and position - cell or cell - position
  if holding then distance = distance + hold_progress end
  if distance >= 0 and distance < trail_length then return distance end
  return nil
end

function M.knight_rider(options, ctx)
  local width = tonumber(options.width) or 8
  local step = tonumber(options.step_ms or options.step) or 75
  local hold = tonumber(options.hold_frames or options.hold) or 9
  local trail = tonumber(options.trail_len or options.trail) or 6
  if width <= 0 then width = 8 end
  if step <= 0 then step = 75 end
  if hold <= 0 then hold = 9 end
  if trail <= 0 then trail = 6 end
  width = math.max(2, math.min(32, math.floor(width)))
  step = math.floor(step)
  hold = math.min(60, math.floor(hold))
  trail = math.floor(trail)
  local started = tonumber(options.started_at_ms or ctx.values.started_at_ms or 0) or 0
  local now = tonumber(ctx.now_ms or 0) or 0
  local elapsed = math.max(0, now - started)
  local cycle = width + hold + width - 1 + hold
  local frame = math.floor(elapsed / step) % cycle
  local position, forward, holding, progress = knight_state(frame, width, hold)
  local colors = options.colors or {}
  local placeholder = options.placeholder_color or options.placeholder or colors[math.floor(#colors / 2) + 1]
  local children = {}
  if options.prefix then children[#children + 1] = options.prefix end
  for cell = 0, width - 1 do
    local distance = trail_index(cell, position, forward, holding, progress, trail)
    local color = distance and colors[math.min(#colors, distance + 1)] or placeholder
    local glyph = distance and (options.head or "■") or (options.empty or "⬝")
    children[#children + 1] = {kind = "text", text = glyph, style = {fg = color, bg = options.bg}}
  end
  if options.suffix then children[#children + 1] = options.suffix end
  return {kind = "row", children = children}, now + step - (elapsed % step)
end

return M
