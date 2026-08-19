local M = {}

local function execute(argv, options)
  local ok, result = pcall(__pixy_host.exec, argv, options)
  if not ok then return nil end
  return result
end

function M.branch(ctx)
  if ctx.values and ctx.values.git_branch ~= nil then
    return type(ctx.values.git_branch) == "string" and ctx.values.git_branch ~= "" and ctx.values.git_branch or nil
  end
  local result = execute({"git", "branch", "--show-current"}, {cwd = ctx.values.cwd, env = ctx.env, timeout_ms = 40, ttl_ms = 250})
  if not result or result.status ~= 0 then return nil end
  local branch = result.stdout:gsub("%s+$", "")
  if branch == "" then return nil end
  return branch
end

function M.status(ctx)
  if ctx.values and ctx.values.git_status ~= nil then
    return (ctx.values.git_status == "clean" or ctx.values.git_status == "dirty") and ctx.values.git_status or nil
  end
  local result = execute({"git", "status", "--porcelain", "--untracked-files=no"}, {cwd = ctx.values.cwd, env = ctx.env, timeout_ms = 60, ttl_ms = 250})
  if not result or result.status ~= 0 then return nil end
  return result.stdout == "" and "clean" or "dirty"
end

return M
