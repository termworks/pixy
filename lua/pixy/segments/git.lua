local M = {}

-- **`--no-optional-locks`, on every git a prompt runs.**
--
-- `git status` takes `.git/index.lock` to refresh the index stat cache on its
-- way past. That is a courtesy to the *next* git command and worth nothing to a
-- prompt -- and a prompt is exactly where it goes wrong: this segment is asked
-- again every `ttl_ms` in every shell that is standing in the repository, and
-- it kills git at `timeout_ms`. A `git status` that overruns in a large tree,
-- or during a build that has just restatted everything, is killed while it
-- holds that lock and leaves the empty file behind. Every real git command in
-- the repository then refuses -- "Another git process seems to be running" --
-- until somebody deletes it by hand. A shell's prompt must not be able to do
-- that to the repository it is describing.
--
-- The flag is git's own answer to this and is documented for it. `branch` does
-- not take the lock today, but it is passed there too: a segment that reads is
-- a segment that reads, and the next git to grow an opportunistic write should
-- not be able to reintroduce this.
local NO_LOCKS = "--no-optional-locks"

local function execute(argv, options)
  local ok, result = pcall(__pixy_host.exec, argv, options)
  if not ok then return nil end
  return result
end

function M.branch(ctx)
  if ctx.values and ctx.values.git_branch ~= nil then
    return type(ctx.values.git_branch) == "string" and ctx.values.git_branch ~= "" and ctx.values.git_branch or nil
  end
  local result = execute({"git", NO_LOCKS, "branch", "--show-current"}, {cwd = ctx.values.cwd, env = ctx.env, timeout_ms = 40, ttl_ms = 250})
  if not result or result.status ~= 0 then return nil end
  local branch = result.stdout:gsub("%s+$", "")
  if branch == "" then return nil end
  return branch
end

function M.status(ctx)
  if ctx.values and ctx.values.git_status ~= nil then
    return (ctx.values.git_status == "clean" or ctx.values.git_status == "dirty") and ctx.values.git_status or nil
  end
  local result = execute({"git", NO_LOCKS, "status", "--porcelain", "--untracked-files=no"}, {cwd = ctx.values.cwd, env = ctx.env, timeout_ms = 60, ttl_ms = 250})
  if not result or result.status ~= 0 then return nil end
  return result.stdout == "" and "clean" or "dirty"
end

return M
