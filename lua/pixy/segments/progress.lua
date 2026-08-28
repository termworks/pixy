-- Progress reported by a host: OSC 9;4 from a shell, a build, a download.
--
-- The caller supplies `progress_state` and `progress_pct`; this module turns
-- them into a node. What the states mean is the host's business, so nothing
-- here invents one: an unknown state draws nothing.

local pixy = require("pixy")
local animate = require("pixy.animate")

local M = {}

M.STATES = {"inactive", "in_progress", "error", "indeterminate", "paused"}

local DEFAULT_COLORS = {
  in_progress = 39,
  error = 203,
  indeterminate = 39,
  paused = 244,
}

local DEFAULT_GLYPHS = {filled = "█", partial = "▓", empty = "░"}

local function value(ctx, name)
  local values = ctx and ctx.values or {}
  return values[name]
end

--- The state a caller reported, or nil when it reported nothing usable.
function M.state(ctx)
  local state = value(ctx, "progress_state")
  if type(state) ~= "string" then return nil end
  for _, known in ipairs(M.STATES) do
    if state == known then
      return state ~= "inactive" and state or nil
    end
  end
  return nil
end

--- The percentage a caller reported, clamped to 0-100, or nil.
function M.percent(ctx)
  local pct = tonumber(value(ctx, "progress_pct"))
  if not pct then return nil end
  return math.max(0, math.min(100, pct))
end

--- A determinate bar: `width` cells, filled in proportion to `percent`.
--- The partial glyph marks the cell the fraction lands inside, so a bar that
--- has not finished a cell still looks different from one that has not started.
function M.bar(options)
  options = options or {}
  local width = math.max(1, math.floor(tonumber(options.width) or 10))
  local percent = math.max(0, math.min(100, tonumber(options.percent) or 0))
  local glyphs = options.glyphs or DEFAULT_GLYPHS
  local exact = percent / 100 * width
  local filled = math.floor(exact)
  local partial = exact - filled >= 0.5 and filled < width
  local text = string.rep(glyphs.filled or DEFAULT_GLYPHS.filled, filled)
  if partial then text = text .. (glyphs.partial or DEFAULT_GLYPHS.partial) end
  local drawn = filled + (partial and 1 or 0)
  text = text .. string.rep(glyphs.empty or DEFAULT_GLYPHS.empty, width - drawn)
  return pixy.text(text, options.style or {fg = DEFAULT_COLORS.in_progress})
end

--- An indeterminate bar: a block that sweeps, because there is no number to
--- draw. Reports its own next frame, so a caller polls when it changes.
function M.sweep(options, ctx)
  options = options or {}
  local width = math.max(2, math.floor(tonumber(options.width) or 10))
  local block = math.max(1, math.min(width - 1, math.floor(tonumber(options.block) or 3)))
  local interval = math.max(1, tonumber(options.interval_ms) or 90)
  local span = (width - block) * 2
  if span <= 0 then span = 1 end
  local now = tonumber(ctx and ctx.now_ms) or 0
  local started = tonumber(options.started_at_ms or value(ctx, "started_at_ms")) or 0
  local elapsed = math.max(0, now - started)
  local frame = math.floor(elapsed / interval) % span
  local offset = frame < (width - block) and frame or span - frame
  local glyphs = options.glyphs or DEFAULT_GLYPHS
  local text = string.rep(glyphs.empty or DEFAULT_GLYPHS.empty, offset)
    .. string.rep(glyphs.filled or DEFAULT_GLYPHS.filled, block)
    .. string.rep(glyphs.empty or DEFAULT_GLYPHS.empty, width - block - offset)
  local node = pixy.text(text, options.style or {fg = DEFAULT_COLORS.indeterminate})
  node.next_frame_ms = interval - (elapsed % interval)
  return node
end

--- The whole thing: nothing when no progress is reported, a sweep when the
--- host cannot say how far along it is, a bar when it can.
function M.segment(options, ctx)
  options = options or {}
  local state = M.state(ctx)
  if not state then return nil end
  local colors = options.colors or DEFAULT_COLORS
  local style = {fg = colors[state] or DEFAULT_COLORS.in_progress}
  if options.bold then style.bold = true end
  if options.bg then style.bg = options.bg end
  local percent = M.percent(ctx)
  if state == "indeterminate" or not percent then
    return M.sweep({
      width = options.width,
      block = options.block,
      interval_ms = options.interval_ms,
      glyphs = options.glyphs,
      style = style,
      started_at_ms = options.started_at_ms,
    }, ctx)
  end
  local bar = M.bar({
    width = options.width,
    percent = percent,
    glyphs = options.glyphs,
    style = style,
  })
  if options.label == false then return bar end
  return pixy.row({bar, pixy.text(string.format(" %d%%", math.floor(percent + 0.5)), style)})
end

--- Frames for a plain spinner, by name. `pixy.spinner{frames = ...}` takes any
--- list; these are the ones people keep rewriting.
M.SPINNERS = {
  dots = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"},
  line = {"-", "\\", "|", "/"},
  bounce = {"⠁", "⠂", "⠄", "⠂"},
  arc = {"◜", "◠", "◝", "◞", "◡", "◟"},
  circle = {"◐", "◓", "◑", "◒"},
  square = {"◰", "◳", "◲", "◱"},
  triangle = {"◢", "◣", "◤", "◥"},
  clock = {"🕐", "🕑", "🕒", "🕓", "🕔", "🕕", "🕖", "🕗", "🕘", "🕙", "🕚", "🕛"},
}

--- One frame of a named spinner, plus the deadline of the next.
function M.spinner(name, options, ctx)
  options = options or {}
  local frames = M.SPINNERS[name]
  if not frames then error("unknown spinner '" .. tostring(name) .. "'") end
  local now = tonumber(ctx and ctx.now_ms) or 0
  local frame, next_frame = animate.frames(
    frames,
    options.interval_ms or 80,
    now,
    options.started_at_ms or value(ctx, "started_at_ms") or 0
  )
  if not frame then return nil end
  local node = pixy.text(frame, options.style or {})
  node.next_frame_ms = next_frame
  return node
end

return M
