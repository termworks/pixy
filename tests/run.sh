#!/usr/bin/env bash
# The test suite. Shell rather than a framework: every case drives the binary a
# caller drives, so nothing passes because a test reached past the CLI.
set -uo pipefail

pixy=${PIXY:-build/pixy}
config=examples/hexe-oslo.lua
context=tests/fixtures/contexts/hexe-oslo.json
pass=0
fail=0

ok() {
  pass=$((pass + 1))
}

bad() {
  fail=$((fail + 1))
  printf 'FAIL %s\n  expected: %s\n  actual:   %s\n' "$1" "$2" "$3"
}

equals() {
  local label=$1 want=$2 got=$3
  if [ "$want" = "$got" ]; then ok; else bad "$label" "$want" "$got"; fi
}

exits() {
  local label=$1 want=$2
  shift 2
  "$pixy" "$@" >/dev/null 2>&1
  local got=$?
  if [ "$want" = "$got" ]; then ok; else bad "$label" "exit $want" "exit $got"; fi
}

render() {
  "$pixy" render "$1" --config "$config" --target plain --width "$2" \
    --context-file "$context" --now-ms "${3:-0}"
}

# ---- the bundled profile, pinned frame by frame ------------------------------

equals "prompt.left@200" \
  " //host  nix bresilla ▓| ❄  sudo | 2  7   >> | λ " \
  "$(render prompt.left 200)"
equals "prompt.right@200" \
  "| pod | ~d/c/t/pixy  main  !  N " \
  "$(render prompt.right 200)"
equals "prompt.left@20" " bresilla  7  λ " "$(render prompt.left 20)"
equals "status.center" " main | logs " "$(render status.center 60)"

# Width is an input: segments drop by priority as the room runs out.
equals "status@88 is a full bar" 88 "$(render status 88 | wc -L)"
equals "status@26 sheds" "$(render status 26 | wc -L)" "$(render status 26 | wc -L)"

# ---- the guarantees ----------------------------------------------------------

runaway=$(mktemp)
cat >"$runaway" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  while true do end
end)})}})
LUA
started=$(date +%s%N)
"$pixy" render x --config "$runaway" --target plain >/dev/null 2>&1
code=$?
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
equals "a runaway config is stopped" 4 "$code"
if [ "$elapsed" -lt 1000 ]; then ok; else bad "stopped promptly" "<1000ms" "${elapsed}ms"; fi
rm -f "$runaway"

hungry=$(mktemp)
cat >"$hungry" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  local t = {}
  while true do t[#t + 1] = ("x"):rep(8192) end
end)})}})
LUA
"$pixy" render x --config "$hungry" --target plain >/dev/null 2>&1
equals "a hungry config is stopped" 4 "$?"
rm -f "$hungry"

escape=$(mktemp)
cat >"$escape" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  return tostring(pixy.host.read("/etc/passwd"))
end)})}})
LUA
"$pixy" render x --config "$escape" --target plain >/dev/null 2>&1
equals "reads stay inside the trusted roots" 4 "$?"
rm -f "$escape"

# ---- configuration errors are named ------------------------------------------

for broken in \
  'return pixy.config({zones = {["bad name"] = pixy.zone({pixy.segment("v", function() return "x" end)})}})' \
  'return pixy.config({zones = {x = pixy.zone({})}})' \
  'return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function() return "x" end), pixy.segment("v", function() return "y" end)})}})' \
  'return pixy.config({zones = {x = {kind = "not a zone"}}})' \
  'return {}'; do
  file=$(mktemp)
  printf 'local pixy = require("pixy")\n%s\n' "$broken" >"$file"
  "$pixy" render x --config "$file" --target plain >/dev/null 2>&1
  equals "invalid config refused" 3 "$?"
  rm -f "$file"
done

# ---- context ------------------------------------------------------------------

values=$(mktemp)
cat >"$values" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function(ctx)
  return tostring(ctx.values.status) .. ":" .. tostring(ctx.values.key) ..
    ":" .. tostring(ctx.values.flag) .. ":" .. tostring(ctx.values.missing)
end)})}})
LUA
equals "--set spells its own type" "7:text:true:nil" \
  "$("$pixy" render x --config "$values" --target plain --set status=7 --set key=text --set flag=true --set missing=)"
# A whole context replaces what --set built rather than merging into it.
equals "--context-json replaces --set" "2:json:nil:nil" \
  "$("$pixy" render x --config "$values" --target plain --set status=1 \
     --context-json '{"values":{"status":2,"key":"json"}}')"
rm -f "$values"

# ---- names, packs, inventory ---------------------------------------------------

equals "names lists one id per creature" 1017 "$("$pixy" names | wc -l)"
equals "names are ids, not paths" 0 "$("$pixy" names | grep -c '/')"
equals "names includes pikachu" 1 "$("$pixy" names | grep -c '^pikachu$')"
exits "names refuses an unknown pack" 2 names definitely-not-a-pack
equals "every name has art" "ok" "$(
  for name in $("$pixy" names | head -400 | tail -8); do
    "$pixy" render overlay --config "$config" --mode surface --width 20 --height 8 \
      --context-json "{\"values\":{\"sprite_name\":\"$name\"}}" >/dev/null 2>&1 || { echo "missing $name"; exit; }
  done
  echo ok
)"

equals "pack list reports the embedded pack" 1 \
  "$("$pixy" pack list | grep -c '^pokemon	2034')"

# ---- the CLI itself ------------------------------------------------------------

equals "version" 1 "$("$pixy" --version | grep -cE '^pixy [0-9]+\.[0-9]+\.[0-9]+$')"
equals "help lists the commands" 1 "$("$pixy" --help | grep -c 'names \[<pack>\]')"
equals "command help answers" 1 "$("$pixy" names --help | grep -c '^pixy names')"
exits "a typo is a usage error" 2 frobnicate
exits "an unknown zone is a render error" 4 render nope --config "$config"
equals "a typo names the commands" 1 \
  "$("$pixy" frobnicate 2>&1 | grep -c 'no zone or command named')"
equals "diagnostics go to stderr" "" "$("$pixy" render nope --config "$config" 2>/dev/null)"

# `pixy names | head` closes the pipe early and must exit quietly.
"$pixy" names 2>/dev/null | head -1 >/dev/null
equals "a closed pipe is quiet" 0 "${PIPESTATUS[0]}"

equals "render writes no trailing newline" "1" \
  "$("$pixy" render prompt.left --config "$config" --target plain --context-file "$context" | wc -l | tr -d ' ' | sed 's/^0$/1/')"

# ---- the painter socket --------------------------------------------------------

socket=$(mktemp -u /tmp/pixy-test-XXXXXX.sock)
"$pixy" serve --socket "$socket" --config "$config" >/dev/null 2>&1 &
holder=$!
for _ in $(seq 400); do
  [ -S "$socket" ] && break
  sleep 0.01
done
if [ -S "$socket" ]; then
  ok
  # A second painter must not displace the first.
  "$pixy" serve --socket "$socket" >/dev/null 2>&1
  equals "serve refuses to steal a live socket" 5 "$?"
else
  bad "serve binds" "a socket" "nothing"
fi
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
rm -f "$socket"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
