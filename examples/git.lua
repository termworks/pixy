local pixy = require("pixy")
local git = require("pixy.segments.git")

-- Providers are ordinary Lua. `git.branch` and `git.status` shell out once and
-- cache the result for `ttl_ms`, so a prompt rendered twice in a second pays
-- for one `git` process. A caller that already knows the answer can pass
-- `--set git_branch=main --set git_status=dirty` and no process runs at all.
local function branch(ctx)
  local name = git.branch(ctx)
  if not name then return nil end
  return pixy.text(" " .. name .. " ", {bg = 4, fg = 0, bold = true})
end

local function dirty(ctx)
  if git.status(ctx) ~= "dirty" then return nil end
  return pixy.text(" ! ", {bg = 3, fg = 0})
end

return pixy.config({
  zones = {
    ["prompt.right"] = pixy.zone({
      pixy.segment("branch", branch, {priority = 1}),
      pixy.segment("dirty", dirty, {priority = 2}),
    }),
  },
})
