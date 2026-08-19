local M = {}

local function node(kind, fields)
  fields = fields or {}
  fields.kind = kind
  return fields
end

function M.text(value, style)
  return node("text", {text = tostring(value or ""), style = style or {}})
end

function M.row(children)
  return node("row", {children = children or {}})
end

function M.segments(children)
  return node("segments", {children = children or {}})
end

function M.regions(options)
  options = options or {}
  return node("regions", {
    left = options.left or {},
    center = options.center or {},
    right = options.right or {},
  })
end

function M.column(children)
  return node("column", {children = children or {}})
end

function M.pad(value, padding)
  if type(padding) == "number" then
    padding = {left = padding, right = padding}
  end
  return node("pad", {value = value, padding = padding or {}})
end

function M.when(condition, value)
  if condition then return value end
  return nil
end

function M.priority(value, priority)
  return node("priority", {value = value, priority = tonumber(priority) or 0})
end

function M.item(value, options)
  options = options or {}
  return node("item", {
    value = value,
    priority = tonumber(options.priority) or 0,
    id = options.id,
    actions = options.actions,
    hover_style = options.hover_style,
    press_styles = options.press_styles,
  })
end

function M.region(value, options)
  options = options or {}
  return node("region", {
    value = value,
    id = options.id,
    actions = options.actions,
    hover_style = options.hover_style,
    press_styles = options.press_styles,
  })
end

function M.truncate(value, width, marker)
  return node("truncate", {value = value, width = tonumber(width) or 0, marker = marker or ""})
end

function M.style(value, style)
  return node("style", {value = value, style = style or {}})
end

function M.palette(colors)
  return colors or {}
end

function M.spinner(options)
  options = options or {}
  options.spinner = options.spinner or options.kind
  return node("spinner", options)
end

function M.animate(callback, interval_ms)
  if type(callback) == "table" then
    return node("animate", callback)
  end
  return node("animate", {callback = callback, interval_ms = interval_ms})
end

function M.surface(lines)
  return node("surface", {lines = lines or {}})
end

function M.transparent(width)
  return node("transparent", {width = math.max(0, tonumber(width) or 0)})
end

return M
