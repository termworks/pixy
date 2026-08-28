local M = {}
local network_samples = {}

local function read(path)
  local ok, data = pcall(__pixy_host.read, path)
  if not ok then return nil end
  return data
end

local function execute(argv, options)
  local ok, result = pcall(__pixy_host.exec, argv, options)
  if not ok then return nil end
  return result
end

local function value(ctx, name, fallback)
  if ctx and ctx.values and ctx.values[name] ~= nil then return ctx.values[name] end
  return fallback
end

function M.parse_linux_uptime(data)
  if type(data) ~= "string" then return nil end
  local uptime = tonumber(data:match("^(%S+)"))
  return uptime and uptime >= 0 and uptime or nil
end

function M.parse_linux_memory(data)
  if type(data) ~= "string" then return nil end
  local total = tonumber(data:match("MemTotal:%s+(%d+)"))
  local available = tonumber(data:match("MemAvailable:%s+(%d+)"))
  if not total or not available or total <= 0 or available > total then return nil end
  return {total_kib = total, used_kib = total - available}
end

local function parse_cpu_values(data)
  if type(data) ~= "string" then return nil end
  local line = data:match("^cpu%s+([^\n]+)")
  if not line or line:find("[^%d%s]") then return nil end
  local values, total = {}, 0
  for number in line:gmatch("%d+") do values[#values + 1] = tonumber(number); total = total + tonumber(number) end
  if #values < 4 then return nil end
  return {total = total, idle = values[4]}
end

function M.parse_linux_cpu(data) return parse_cpu_values(data) end
function M.parse_macos_cpu(data)
  if type(data) ~= "string" or data:find("[^%d%s]") then return nil end
  local values, total = {}, 0
  for number in data:gmatch("%d+") do values[#values + 1] = tonumber(number); total = total + tonumber(number) end
  if #values < 5 then return nil end
  return {total = total, idle = values[5]}
end

function M.parse_linux_network(data)
  if type(data) ~= "string" then return nil end
  local received, sent, found = 0, 0, false
  for line in data:gmatch("[^\n]+") do
    local name, rx, tx = line:match("^%s*([^:]+):%s*(%d+)%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+(%d+)")
    if name and name ~= "lo" then found = true; received = received + tonumber(rx); sent = sent + tonumber(tx) end
  end
  if not found then return nil end
  return {received = received, sent = sent}
end

function M.parse_macos_network(data)
  if type(data) ~= "string" then return nil end
  local received, sent, found = 0, 0, false
  for line in data:gmatch("[^\n]+") do
    local fields = {}
    for field in line:gmatch("%S+") do fields[#fields + 1] = field end
    local input, output = tonumber(fields[7]), tonumber(fields[10])
    if fields[1] and fields[1] ~= "Name" and fields[1] ~= "lo0" and input and output then
      found = true; received = received + input; sent = sent + output
    end
  end
  if not found then return nil end
  return {received = received, sent = sent}
end

function M.parse_macos_boottime(data, now_seconds)
  if type(data) ~= "string" then return nil end
  local boot = tonumber(data:match("sec%s*=%s*(%d+)"))
  if not boot or boot > now_seconds then return nil end
  return now_seconds - boot
end

function M.parse_macos_memory(data)
  if type(data) ~= "string" then return nil end
  local total = tonumber(data:match("^%s*(%d+)%s*$"))
  return total and {total_bytes = total} or nil
end

function M.parse_macos_battery(data)
  if type(data) ~= "string" then return nil end
  local percent = tonumber(data:match("(%d+)%%"))
  if not percent or percent < 0 or percent > 100 then return nil end
  return {percent = percent, status = data:match(";%s*([^;]+);")}
end

function M.time(ctx)
  return math.floor((tonumber(ctx and ctx.now_ms) or 0) / 1000)
end

-- The clock pixy is already holding, formatted in process. Running `date`
-- instead costs a fork a prompt, ignores a pinned --now-ms, and reads the zone
-- with whatever libc happens to be first on PATH -- which is how a prompt ends
-- up hours off when that one cannot find its zoneinfo and quietly falls back to
-- UTC.
function M.clock(ctx, format)
  return os.date(format or "%H:%M:%S", M.time(ctx))
end

function M.hostname()
  return __pixy_host.env("HOSTNAME")
end

function M.username()
  return __pixy_host.env("USER") or __pixy_host.env("LOGNAME")
end

function M.uptime(ctx)
  local data = read(value(ctx, "uptime_path", "/proc/uptime"))
  if not data then
    if __pixy_host.platform ~= "macos" or (ctx and ctx.values and ctx.values.uptime_path) then return nil end
    local result = execute({"sysctl", "-n", "kern.boottime"}, {timeout_ms = 40, ttl_ms = 1000})
    if not result or result.status ~= 0 then return nil end
    return M.parse_macos_boottime(result.stdout, M.time(ctx))
  end
  return M.parse_linux_uptime(data)
end

function M.memory(ctx)
  local data = read(value(ctx, "meminfo_path", "/proc/meminfo"))
  if not data then
    if __pixy_host.platform ~= "macos" or (ctx and ctx.values and ctx.values.meminfo_path) then return nil end
    local result = execute({"sysctl", "-n", "hw.memsize"}, {timeout_ms = 40, ttl_ms = 1000})
    if not result or result.status ~= 0 then return nil end
    return M.parse_macos_memory(result.stdout)
  end
  return M.parse_linux_memory(data)
end

function M.cpu(ctx)
  local data = read(value(ctx, "stat_path", "/proc/stat"))
  if not data then
    if __pixy_host.platform ~= "macos" or (ctx and ctx.values and ctx.values.stat_path) then return nil end
    local result = execute({"sysctl", "-n", "kern.cp_time"}, {timeout_ms = 40, ttl_ms = 250})
    if not result or result.status ~= 0 then return nil end
    return M.parse_macos_cpu(result.stdout)
  end
  return M.parse_linux_cpu(data)
end

function M.battery(ctx)
  local capacity = read(value(ctx, "battery_capacity_path", "/sys/class/power_supply/BAT0/capacity"))
  if not capacity then
    if __pixy_host.platform ~= "macos" or (ctx and ctx.values and ctx.values.battery_capacity_path) then return nil end
    local result = execute({"pmset", "-g", "batt"}, {timeout_ms = 50, ttl_ms = 1000})
    if not result or result.status ~= 0 then return nil end
    return M.parse_macos_battery(result.stdout)
  end
  local percent = tonumber(capacity:match("^%s*(%d+)%s*$"))
  if not percent or percent < 0 or percent > 100 then return nil end
  local status = read(value(ctx, "battery_status_path", "/sys/class/power_supply/BAT0/status"))
  return {percent = percent, status = status and status:gsub("%s+$", "") or nil}
end

function M.network(ctx)
  local data = read(value(ctx, "netdev_path", "/proc/net/dev"))
  if not data then
    if __pixy_host.platform ~= "macos" or (ctx and ctx.values and ctx.values.netdev_path) then return nil end
    local result = execute({"netstat", "-ibn"}, {timeout_ms = 50, ttl_ms = 250})
    if not result or result.status ~= 0 then return nil end
    return M.parse_macos_network(result.stdout)
  end
  return M.parse_linux_network(data)
end

function M.network_speed(ctx)
  local current = M.network(ctx)
  if not current then return nil end
  local now = tonumber(ctx and ctx.now_ms) or 0
  local key = tostring(value(ctx, "netdev_path", __pixy_host.platform))
  local previous = network_samples[key]
  network_samples[key] = {received = current.received, sent = current.sent, now = now}
  if not previous or now <= previous.now or current.received < previous.received or current.sent < previous.sent then return nil end
  local seconds = (now - previous.now) / 1000
  return {
    received_per_second = (current.received - previous.received) / seconds,
    sent_per_second = (current.sent - previous.sent) / seconds,
  }
end

function M.sudo(ctx, options)
  if ctx and ctx.values and ctx.values.sudo ~= nil then return ctx.values.sudo end
  local result = execute({"sudo", "-n", "true"}, {env = ctx and ctx.env, timeout_ms = 30, ttl_ms = tonumber(options and options.ttl_ms) or 5000})
  if not result or result.timed_out then return nil end
  return result.status == 0
end

--- One of `values`, held steady for as long as the thing it labels is running.
---
--- Seeded from the run's start, NOT from the clock. A status bar redraws for
--- whatever else is on it -- a spinner beside this one asks for a frame every
--- few tens of milliseconds -- and seeding from `now_ms` re-rolled the choice on
--- every one of those frames, so the word churned instead of labelling anything.
--- The spinner is what moves; the word beside it is what stays.
---
--- `seed` overrides, for a caller whose run has some other identity. Failing
--- both, the clock is the last resort and behaves as before -- churn is better
--- than always answering with the first entry.
function M.random(ctx, values, seed)
  if not values or #values == 0 then return nil end
  local run = seed
    or (ctx and ctx.values and ctx.values.started_at_ms)
    or (ctx and ctx.now_ms)
    or 0
  return values[(math.floor(tonumber(run) or 0) % #values) + 1]
end

return M
