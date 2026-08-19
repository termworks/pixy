local pixy = require("pixy")
local shell = require("pixy.segments.shell")
local git = require("pixy.segments.git")

local function pokemon(ctx, force_shiny)
  local values = ctx.values or {}
  local name = tostring(values.sprite_name or values.pokemon_name or "pikachu")
  local shiny = force_shiny or values.sprite_shiny == true
  local prefix = shiny and "shiny/" or "regular/"
  return pixy.sprite({
    pack = "pokemon",
    name = prefix .. name,
    fallback_name = prefix .. "pikachu",
    format = "ansi",
    position = values.sprite_position or "center",
    transparent = true,
  })
end

return pixy.config({
  zones = {
    demo = pixy.zone({
      pixy.segment("pixy1", function()
        return pixy.text(" pixy ", {fg = 15, bg = 237, bold = true})
      end),
      pixy.segment("pixy2", function(ctx)
        if (ctx.values.status or 0) == 0 then return nil end
        return pixy.text(" " .. tostring(ctx.values.status) .. " ", {fg = 9, bg = 0})
      end),
    }),
    ["prompt.left"] = pixy.zone({
      pixy.segment("directory", function(ctx)
        return pixy.text(" " .. (shell.directory(ctx) or "?") .. " ", {fg = 15, bg = 24, bold = true})
      end),
      pixy.segment("status", function(ctx)
        local status = tonumber(shell.status(ctx) or 0) or 0
        if status == 0 then return nil end
        return pixy.text(" " .. tostring(status) .. " ", {fg = 15, bg = 1})
      end),
      pixy.segment("git", function(ctx)
        local branch = git.branch(ctx)
        return branch and pixy.text(" " .. branch .. " ", {fg = 0, bg = 6}) or nil
      end),
      pixy.segment("space", function() return " " end),
      pixy.segment("character", shell.character),
    }),
    ["prompt.right"] = pixy.zone({
      pixy.segment("language", function(ctx)
        return ctx.values.language and pixy.text(" " .. ctx.values.language .. " ", {fg = 14}) or nil
      end),
      pixy.segment("jobs", function(ctx)
        return (ctx.values.jobs or 0) > 0 and pixy.text(" jobs:" .. tostring(ctx.values.jobs) .. " ", {fg = 11}) or nil
      end),
      pixy.segment("duration", function(ctx)
        return ctx.values.duration_ms and ctx.values.duration_ms > 0 and pixy.text(" " .. tostring(ctx.values.duration_ms) .. "ms ", {fg = 8}) or nil
      end),
      pixy.segment("vimode", function(ctx)
        return ctx.values.vimode and pixy.text(" " .. ctx.values.vimode .. " ", {fg = 13}) or nil
      end),
    }),
    activity = pixy.zone({
      pixy.segment("spinner", function(ctx)
        return pixy.spinner({frames = {"⠋", "⠙", "⠹", "⠸"}, interval_ms = 80, started_at_ms = ctx.values.started_at_ms})
      end),
    }),
    mascot = pixy.zone({
      pixy.segment("sprite", function(ctx)
        return pixy.sprite({frames = {" /\\\n<  >\n \\/", " \\/\n<  >\n /\\"}, interval_ms = 200, started_at_ms = ctx.values.started_at_ms})
      end),
    }),
    pokemon = pixy.zone({
      pixy.segment("sprite", function(ctx) return pokemon(ctx, false) end),
    }),
    ["pokemon.shiny"] = pixy.zone({
      pixy.segment("sprite", function(ctx) return pokemon(ctx, true) end),
    }),
  },
})
