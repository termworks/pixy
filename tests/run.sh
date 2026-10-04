#!/usr/bin/env bash
set -uo pipefail

pixy=${PIXY:-build/pixy}
# Stretching is on by default and detected from the environment, so a run inside
# a hexe pane would otherwise answer differently from one outside it. Tests that
# want it say so.
unset HEXE_STRETCH
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { printf 'FAIL %s\n  expected: %s\n  actual:   %s\n' "$1" "$2" "$3"; fail=$((fail + 1)); }
equals() { if [ "$2" = "$3" ]; then ok; else bad "$1" "$2" "$3"; fi; }
exits() {
  local label=$1 expected=$2
  shift 2
  "$pixy" "$@" >/dev/null 2>&1
  equals "$label" "$expected" "$?"
}

cat >"$tmp/init.lua" <<'LUA'
local p = require("pixy")
local progress = p.progress
local system = p.system

p.zone("line", {
  p.segment("left", function(ctx)
    return p.text(" " .. tostring(ctx.values.name or "pixy") .. " ", {fg = 15, bg = 24, bold = true})
  end, {priority = 1}),
  p.segment("status", function(ctx)
    return p.when((ctx.values.status or 0) ~= 0,
      p.text(" " .. tostring(ctx.values.status) .. " ", {fg = "white", bg = "red"}))
  end, {priority = 5}),
})

p.zone("layout", {
  p.segment("content", function()
    return p.row({p.text("left"), p.spacer(), p.style("right", {fg = {1, 2, 3}})})
  end),
})

p.zone("fill", {
  p.segment("content", function()
    return p.row({p.text("L"), p.spacer({fill = "-="}), p.text("R")})
  end),
})

p.zone("widefill", {
  p.segment("content", function()
    return p.row({p.text("L"), p.spacer({fill = "漢"}), p.text("R")})
  end),
})

p.zone("animation", {
  p.segment("spinner", function(ctx)
    return p.spinner({frames = {"a", "b", "c"}, interval_ms = 50,
      started_at_ms = ctx.values.started_at_ms})
  end),
})

p.zone("surface", {
  p.segment("sprite", function()
    return p.sprite({frames = {" x \nyy "}, transparent = true})
  end),
})

p.zone("host", {
  p.segment("exec", function()
    local result = p.host.exec({"printf", "native"}, {timeout_ms = 100})
    return result.stdout
  end),
})

p.zone("builtin", {
  p.segment("directory", p.renderers.directory),
  p.segment("git", p.renderers.git),
  p.segment("status", p.renderers.status),
  p.segment("spinner", p.renderers.spinner),
})

p.zone("pokemon", {
  p.segment("sprite", p.renderers.pokemon),
})

p.zone("progress", {
  p.segment("bar", function(ctx) return progress.segment({width = 5}, ctx) end),
})

p.zone("system", {
  p.segment("uptime", function(ctx) return tostring(math.floor(system.uptime(ctx))) end),
})

p.zone("providers", {
  p.segment("providers", function(ctx)
    local battery = system.battery(ctx)
    return tostring(system.sudo(ctx)) .. ":" .. tostring(battery.percent)
      .. ":" .. tostring(battery.status) .. ":" .. system.clock(ctx, "!%S")
  end),
})

p.palette({slot = 2, [1] = "#cc241d", bg = "#282828"})
LUA

export PIXY_CACHE_DIR="$tmp/cache"
config=(--config "$tmp/init.lua")

equals "check" 0 "$("$pixy" check "${config[@]}" >/dev/null; echo $?)"
equals "line plain" " pixy " "$("$pixy" render line "${config[@]}" --target plain)"
equals "context" " nova  7 " \
  "$("$pixy" render line "${config[@]}" --target plain --set name=nova --set status=7)"
equals "segment" " 7 " \
  "$("$pixy" render line.status "${config[@]}" --target plain --set status=7)"
equals "width pruning" " nova " \
  "$("$pixy" render line "${config[@]}" --target plain --width 6 --set name=nova --set status=7)"
equals "spacer" "left           right" \
  "$("$pixy" render layout "${config[@]}" --target plain --width 20)"
equals "spacer fill" "L-=-=-=-=R" \
  "$("$pixy" render fill "${config[@]}" --target plain --width 10)"
equals "spacer fill ansi" "L-=-=-=-=R" \
  "$("$pixy" render fill "${config[@]}" --target ansi --width 10)"
# Nothing is asked for: the pane answers. $HEXE_STRETCH is set by a frontend
# that draws OSC 1332, and by nothing else.
stretched=$'\e]1332;begin\e\\L\e]1332;fill;-=\e\\R\e]1332;end\e\\'
equals "stretch under hexe" "$stretched" \
  "$(HEXE_STRETCH=1332 "$pixy" render fill "${config[@]}" --target ansi --width 10)"
equals "stretch without hexe" "L-=-=-=-=R" \
  "$(env -u HEXE_STRETCH "$pixy" render fill "${config[@]}" --target ansi --width 10)"
equals "stretch stays off the plain target" "L-=-=-=-=R" \
  "$(HEXE_STRETCH=1332 "$pixy" render fill "${config[@]}" --target plain --width 10)"
exits "no stretch flag to reject" 2 render fill "${config[@]}" --stretch always
exits "wide spacer fill" 4 render widefill "${config[@]}" --target plain
equals "spinner frame zero" a \
  "$("$pixy" render animation "${config[@]}" --target plain --now-ms 0)"
equals "spinner frame one" b \
  "$("$pixy" render animation "${config[@]}" --target plain --now-ms 50)"
equals "host exec" native "$("$pixy" render host "${config[@]}" --target plain)"
equals "builtin renderers" " /tmp  main  7 ⠋" \
  "$("$pixy" render builtin "${config[@]}" --target plain --set cwd=/tmp --set git_branch=main --set status=7 --now-ms 0)"
equals "native progress" "██▓░░ 50%" \
  "$("$pixy" render progress "${config[@]}" --target plain --set progress_state=in_progress --set progress_pct=50)"
case $("$pixy" render system "${config[@]}" --target plain) in
  ''|*[!0-9]*) bad "native system provider" "uptime seconds" "invalid" ;;
  *) ok ;;
esac

printf '73\n' >"$tmp/capacity"
printf 'Charging\n' >"$tmp/status"
equals "native providers" "true:73:Charging:00" \
  "$("$pixy" render providers "${config[@]}" --target plain --now-ms 50 \
     --context-json "{\"values\":{\"sudo\":true,\"battery_capacity_path\":\"$tmp/capacity\",\"battery_status_path\":\"$tmp/status\"}}")"

ansi=$("$pixy" render line.left "${config[@]}" --target ansi)
case $ansi in *$'\e[38;5;15;48;5;24;1m'*) ok ;; *) bad "ANSI style" "styled" "$(printf %q "$ansi")" ;; esac
bash_prompt=$("$pixy" render line.left "${config[@]}" --target bash)
case $bash_prompt in *'\['*) ok ;; *) bad "bash escape" "wrapped" "$bash_prompt" ;; esac
zsh_prompt=$("$pixy" render line.left "${config[@]}" --target zsh)
case $zsh_prompt in *'%{'*) ok ;; *) bad "zsh escape" "wrapped" "$zsh_prompt" ;; esac

run=$("$pixy" render line.left "${config[@]}" --mode run)
case $run in *'"style":"fg:15 bg:24 bold"'*) ok ;; *) bad "run style" "style descriptor" "$run" ;; esac
surface=$("$pixy" render surface "${config[@]}" --mode surface --width 5 --height 2)
case $surface in *$'\e[1C'*$'\r\n'*) ok ;; *) bad "surface" "two rows with transparency" "$surface" ;; esac

names=$("$pixy" list "${config[@]}")
case $names in *line.left*pokemon.sprite*) ok ;; *) bad "inventory" "zone and segment names" "$names" ;; esac

palette=$("$pixy" palette set "${config[@]}")
case $palette in *'cc241d'*'282828'*) ok ;; *) bad "config palette" "configured colours" "$(printf %q "$palette")" ;; esac

"$pixy" init bash >"$tmp/bash"
"$pixy" init zsh >"$tmp/zsh"
"$pixy" init fish >"$tmp/fish"
grep -q -- '--target bash' "$tmp/bash" && ok || bad "bash init" "bash target" "missing"
grep -q -- '--target zsh' "$tmp/zsh" && ok || bad "zsh init" "zsh target" "missing"
grep -q -- '--target ansi' "$tmp/fish" && ok || bad "fish init" "ansi target" "missing"
exits "unsupported shell" 2 init unknown

"$pixy" render pokemon "${config[@]}" --mode surface --width 80 --height 40 >"$tmp/pokemon"
grep -Fq $'\e[38;2;' "$tmp/pokemon" && ok || bad "embedded pokemon" "truecolor sprite" "missing"
equals "embedded names" 1017 "$("$pixy" names pokemon | wc -l)"

mkdir "$tmp/pack"
printf cat >"$tmp/pack/cat.txt"
"$pixy" pack build "$tmp/pack" --output "$tmp/example.pixypack" \
  --source tests --license MIT --attribution Pixy
"$pixy" pack check "$tmp/example.pixypack" >/dev/null && ok || bad "pack check" 0 "$?"

exits "missing config" 3 render line --config "$tmp/missing.lua" --target plain
exits "unknown selector" 4 render missing "${config[@]}" --target plain

for legacy in \
  'require("pixy.animate")' \
  'require("pixy.segments.git")' \
  'local p=require("pixy"); p.config({zones={}})' \
  'local p=require("pixy"); p.zone({})' \
  'local p=require("pixy"); p.zone("x", {p.segment("s", function() return "x" end)}); return {}'; do
  printf '%s\n' "$legacy" >"$tmp/legacy.lua"
  exits "legacy Lua API rejected" 3 check --config "$tmp/legacy.lua"
done

cat >"$tmp/runaway.lua" <<'LUA'
local p=require("pixy")
p.zone("x", {p.segment("loop", function() while true do end end)})
LUA
exits "render deadline" 4 render x --config "$tmp/runaway.lua" --target plain

cat >"$tmp/render-errors.lua" <<'LUA'
local p=require("pixy")
p.zone("good", {p.segment("first", function() return p.text("ok") end, {id="first"})})
p.zone("callback", {
  p.segment("first", function() return p.text("allocated") end, {id="first"}),
  p.segment("error", function() error("deliberate") end),
})
p.zone("nested", {p.segment("error", function()
  return p.row({p.text("allocated"), p.spacer({fill="漢"})})
end)})
p.zone("sprite", {p.segment("error", function()
  return p.sprite({frames={"valid\n\27]bad"}})
end)})
p.zone("loop", {p.segment("error", function() while true do end end)})
LUA
for zone in callback nested sprite loop; do
  exits "render cleanup: $zone" 4 render "good,$zone" \
    --config "$tmp/render-errors.lua" --target plain
done

frame() {
  local payload=$1 length=${#1}
  printf -v prefix '\\%03o\\%03o\\%03o\\%03o' \
    "$((length >> 24 & 255))" "$((length >> 16 & 255))" \
    "$((length >> 8 & 255))" "$((length & 255))"
  printf '%b%s' "$prefix" "$payload"
}
for iteration in 1 2 3; do
  for zone in good callback nested sprite loop good; do
    frame "{\"select\":[\"$zone\"]}"
  done
done >"$tmp/requests"
"$pixy" serve --stdio --config "$tmp/render-errors.lua" \
  <"$tmp/requests" >"$tmp/responses" 2>"$tmp/serve-errors"
equals "render cleanup: server exit" 0 "$?"
equals "render cleanup: recovered renders" 6 "$(grep -ao '"ok":true' "$tmp/responses" | wc -l)"
equals "render cleanup: failed renders" 12 "$(grep -ao '"ok":false' "$tmp/responses" | wc -l)"

if strings "$pixy" | grep -Eq 'lua/pixy/(layout|style|nodes)\.lua'; then
  bad "no bundled Lua implementation" "no internal module paths" "found"
else
  ok
fi

printf '%d passed, %d failed\n' "$pass" "$fail"
test "$fail" -eq 0
