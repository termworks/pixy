-- Git status: one zone that says everything about the working tree. Counts
-- come from the caller when it knows them (`--set git_staged=2`), and fall back
-- to the bundled provider's clean/dirty answer when it does not.
local pixy = require("pixy")
local git = require("pixy.segments.git")

local marks = {
  {key = "git_staged", glyph = "+", fg = 2},
  {key = "git_unstaged", glyph = "!", fg = 3},
  {key = "git_untracked", glyph = "?", fg = 4},
  {key = "git_stashed", glyph = "$", fg = 5},
  {key = "git_conflicted", glyph = "=", fg = 1},
}

local function count(ctx, key)
  local value = tonumber(ctx.values[key])
  if not value or value <= 0 then return nil end
  return value
end

return pixy.config({
  zones = {
    ["prompt.left"] = pixy.zone({
      pixy.segment("branch", function(ctx)
        local branch = git.branch(ctx)
        if not branch then return nil end
        return pixy.text(" " .. utf8.char(0xE725) .. " " .. branch .. " ", {bg = 236, fg = 114, bold = true})
      end, {priority = 1}),
      pixy.segment("divergence", function(ctx)
        local ahead, behind = count(ctx, "git_ahead"), count(ctx, "git_behind")
        if not ahead and not behind then return nil end
        local text = ""
        if behind then text = text .. "⇣" .. behind end
        if ahead then text = text .. (text == "" and "" or " ") .. "⇡" .. ahead end
        return pixy.text(" " .. text .. " ", {bg = 236, fg = 111})
      end, {priority = 2}),
      pixy.segment("marks", function(ctx)
        local children = {}
        for _, mark in ipairs(marks) do
          local total = count(ctx, mark.key)
          if total then
            children[#children + 1] = pixy.text(" " .. mark.glyph .. total, {bg = 236, fg = mark.fg})
          end
        end
        if #children == 0 then
          if git.status(ctx) ~= "dirty" then return nil end
          return pixy.text(" ● ", {bg = 236, fg = 3})
        end
        children[#children + 1] = pixy.text(" ", {bg = 236})
        return pixy.row(children)
      end, {priority = 3}),
      pixy.segment("mark", function(ctx)
        local failed = (tonumber(ctx.values.status) or 0) ~= 0
        return pixy.text(" ❯ ", {fg = failed and 203 or 114, bold = true})
      end, {priority = 99}),
    }),
  },
})
