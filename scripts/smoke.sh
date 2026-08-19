#!/usr/bin/env bash
set -euo pipefail

pixy=${PIXY_BIN:-target/debug/pixy}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export PIXY_CONFIG="$PWD/lua/pixy/default.lua"
export PIXY_CACHE_DIR="$tmp/cache"

"$pixy" render demo --target plain --set status=7 >"$tmp/render"
"$pixy" demo --target plain --set status=7 >"$tmp/shorthand"
cmp "$tmp/render" "$tmp/shorthand"
test "$(cat "$tmp/render")" = " pixy  7 "
"$pixy" render demo.pixy1,demo.pixy1 --target plain >"$tmp/duplicate"
test "$(cat "$tmp/duplicate")" = " pixy  pixy "

set +e
"$pixy" render missing --target plain >"$tmp/missing.out" 2>"$tmp/missing.err"
code=$?
set -e
test "$code" -eq 4
test ! -s "$tmp/missing.out"
test -s "$tmp/missing.err"

test "$("$pixy" render demo.pixy1 --target plain)" = " pixy "
ansi=$("$pixy" render demo.pixy1 --target ansi)
[[ "$ansi" == *$'\e['* ]]
bash_prompt=$("$pixy" render demo.pixy1 --target bash)
[[ "$bash_prompt" == *'\['* ]]
zsh_prompt=$("$pixy" render demo.pixy1 --target zsh)
[[ "$zsh_prompt" == *'%{'* ]]
"$pixy" render demo.pixy1 --mode run >"$tmp/run.json"
grep -q '"mode":"run"' "$tmp/run.json"
"$pixy" render mascot --mode surface --width 8 --height 2 --now-ms 0 >"$tmp/surface"
test -s "$tmp/surface"

printf '%s' '{"version":1,"select":["demo.pixy1"],"mode":"line","target":"plain","width":80,"height":1,"now_ms":0,"context":{}}' |
  "$pixy" render --request - >"$tmp/request"
test "$(cat "$tmp/request")" = " pixy "

"$pixy" stream activity.spinner --target plain --duration 0 --now-ms 0 >"$tmp/stream"
test -s "$tmp/stream"
"$pixy" stream activity.spinner --target plain --duration 250 --fps 120 >"$tmp/stream-bounded"
stream_updates=$(tr -cd '\r' <"$tmp/stream-bounded" | wc -c)
test "$stream_updates" -ge 2
test "$stream_updates" -le 5
"$pixy" stream mascot --mode surface --width 8 --height 2 --duration 250 --fps 120 >"$tmp/surface-stream"
grep -Fq $'\e[1A\r' "$tmp/surface-stream"

mkdir -p "$tmp/assets/regular" "$tmp/packs"
printf 'cat' >"$tmp/assets/cat.txt"
printf ' \033[38;2;246;213;49m▄\033[48;2;197;164;41m▀\033[0m ' >"$tmp/assets/regular/pikachu"
"$pixy" pack build "$tmp/assets" --output "$tmp/packs/fixture.pixypack" --source smoke --license MIT --attribution Pixy
"$pixy" pack check "$tmp/packs/fixture.pixypack" >"$tmp/pack-check"
"$pixy" pack list "$tmp/packs/fixture.pixypack" >"$tmp/pack-list"
grep -q $'cat.txt\t3\t' "$tmp/pack-list"
grep -q $'regular/pikachu\t' "$tmp/pack-list"
"$pixy" pack list >"$tmp/installed-packs"
grep -q $'^pokemon\t2034\t.*(embedded)$' "$tmp/installed-packs"
cat >"$tmp/assets.lua" <<'LUA'
local pixy=require("pixy")
return pixy.config({zones={
  asset=pixy.zone({pixy.segment("sprite",function() return pixy.sprite({pack="fixture",name="cat.txt"}) end)}),
  pokemon=pixy.zone({pixy.segment("sprite",function() return pixy.sprite({pack="fixture",name="regular/pikachu",format="ansi"}) end)}),
}})
LUA
PIXY_DATA_DIR="$tmp/packs" "$pixy" render asset --target plain --config "$tmp/assets.lua" >"$tmp/asset-render"
test "$(cat "$tmp/asset-render")" = "cat"
PIXY_DATA_DIR="$tmp/packs" "$pixy" render pokemon --mode surface --width 4 --height 1 --config "$tmp/assets.lua" >"$tmp/pokemon-render"
grep -Fq $'\e[38;2;246;213;49m' "$tmp/pokemon-render"
grep -Fq '▀' "$tmp/pokemon-render"

PIXY_DATA_DIR="$tmp/empty-packs" "$pixy" render pokemon --mode surface --width 80 --height 40 >"$tmp/embedded-render"
grep -Fq $'\e[38;2;' "$tmp/embedded-render"
grep -Fq '▀' "$tmp/embedded-render"
PIXY_DATA_DIR="$tmp/empty-packs" "$pixy" render pokemon.shiny --mode surface --width 80 --height 40 >"$tmp/embedded-shiny-render"
grep -Fq $'\e[38;2;' "$tmp/embedded-shiny-render"
grep -Fq '▀' "$tmp/embedded-shiny-render"

printf 'smoke ok\n'
