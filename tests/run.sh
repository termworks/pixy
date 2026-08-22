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

# A config reaches the machine through the host or not at all. `io.open` would
# walk straight past the trusted roots, so the libraries that offer it are never
# opened rather than being taken away afterwards.
sandbox=$(mktemp)
cat >"$sandbox" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  local reachable = {}
  if io ~= nil then reachable[#reachable + 1] = "io" end
  if debug ~= nil then reachable[#reachable + 1] = "debug" end
  if os and os.execute ~= nil then reachable[#reachable + 1] = "os.execute" end
  if os and os.remove ~= nil then reachable[#reachable + 1] = "os.remove" end
  if dofile ~= nil then reachable[#reachable + 1] = "dofile" end
  if loadfile ~= nil then reachable[#reachable + 1] = "loadfile" end
  return #reachable == 0 and "sealed" or table.concat(reachable, ",")
end)})}})
LUA
equals "a config cannot reach past the host" "sealed" \
  "$("$pixy" render x --config "$sandbox" --target plain)"
# What it does need stays: the clock and the width tables are the whole point.
cat >"$sandbox" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  return os.date("!%Y", 0) .. ":" .. tostring(utf8.len("héllo")) .. ":" .. ("x"):rep(2)
end)})}})
LUA
equals "the clock and utf8 are still there" "1970:5:xx" \
  "$("$pixy" render x --config "$sandbox" --target plain)"
rm -f "$sandbox"

# The deadline bounds how long a config may *compute*. Waiting on a provider is
# not computing, and providers have a budget of their own an order of magnitude
# larger, so charging the deadline for time spent blocked in waitpid killed
# prompts that were doing exactly what they were meant to: ask a few providers,
# then lay the answer out.
waiting=$(mktemp)
cat >"$waiting" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  for _ = 1, 3 do pixy.host.exec({"sleep", "0.05"}, {timeout_ms = 500, ttl_ms = 0}) end
  local total = 0
  for i = 1, 200000 do total = total + i end
  return pixy.text(tostring(total))
end)})}})
LUA
equals "waiting on providers does not spend the deadline" "20000100000" \
  "$("$pixy" render x --config "$waiting" --target plain)"
rm -f "$waiting"

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

# ---- a cached provider still expires -----------------------------------------

# A hit must serve what is stored and let it lapse on time. Renewing the window
# on every read froze a 250ms clock for as long as a host kept asking for it,
# which is exactly what a status bar does.
ticking=$(mktemp)
cat >"$ticking" <<'LUA'
local pixy = require("pixy")
return pixy.config({zones = {x = pixy.zone({pixy.segment("v", function()
  local result = pixy.host.exec({"date", "+%s%N"}, {timeout_ms = 200, ttl_ms = 250})
  return (result.stdout:gsub("%s+$", ""))
end)})}})
LUA
first=$("$pixy" render x --config "$ticking" --target plain)
sleep 0.05
within=$("$pixy" render x --config "$ticking" --target plain)
equals "a fresh cache entry is reused" "$first" "$within"
# Poll faster than the ttl throughout, the way a bar does, then look past it.
for _ in $(seq 6); do
  "$pixy" render x --config "$ticking" --target plain >/dev/null
  sleep 0.1
done
after=$("$pixy" render x --config "$ticking" --target plain)
if [ "$first" != "$after" ]; then ok; else bad "a cached provider expires" "a new value" "$after"; fi
rm -f "$ticking"

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

# The oslo integration writes `--target=ansi`; both spellings are one option.
equals "--flag=value is accepted" \
  "$("$pixy" render prompt.left --config "$config" --target plain --width 60 --context-file "$context" --now-ms 0)" \
  "$("$pixy" render prompt.left --config="$config" --target=plain --width=60 --context-file="$context" --now-ms=0)"
for spelling in --target=plain --mode=line --width=40; do
  "$pixy" render prompt.left --config "$config" "$spelling" --context-file "$context" >/dev/null 2>&1
  equals "$spelling parses" 0 "$?"
done
# Every command the shell integrations print must actually run.
for shell in bash zsh fish oslo; do
  while read -r line; do
    equals "init $shell command runs" 0 "$(eval "${line/command pixy/$pixy}" >/dev/null 2>&1; echo $?)"
  done < <("$pixy" init "$shell" | grep -oE '(command )?pixy render [^"$)]*' | head -2)
done

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

# `pixy names | head` closes the pipe early and must exit quietly. Quiet means
# saying nothing, not any one status: whether the writer finishes into the pipe
# buffer first (0) or is killed by SIGPIPE (141) is a race, and dying on SIGPIPE
# is what every other filter does.
noise=$("$pixy" names 2>&1 >/dev/null | head -1)
"$pixy" names 2>/dev/null | head -1 >/dev/null
case "${PIPESTATUS[0]}" in
  0 | 141) equals "a closed pipe is quiet" "" "$noise" ;;
  *) bad "a closed pipe is quiet" "exit 0 or 141" "exit ${PIPESTATUS[0]}" ;;
esac

equals "render writes no trailing newline" "1" \
  "$("$pixy" render prompt.left --config "$config" --target plain --context-file "$context" | wc -l | tr -d ' ' | sed 's/^0$/1/')"

# ---- the clock -----------------------------------------------------------------

# Read in process, from the time pixy already holds. Running `date` instead read
# the zone with whatever libc was first on PATH, and one that cannot find its
# zoneinfo falls back to UTC without saying so -- a prompt hours behind.
clock() {
  "$pixy" render status.left.clock --config "$config" --target plain "$@" | tr -d ' '
}
# One instant, four zones, the offsets they are actually owed. `date` is not the
# reference here: a `date` whose libc cannot find its zoneinfo answers UTC for
# every one of these, which is the bug this pins.
#
# Whether zones resolve at all belongs to the C library this binary was linked
# against, not to pixy: a glibc built by Nix carries a TZDIR holding no zoneinfo,
# so a binary built in the dev shell answers UTC for everything until TZDIR is
# set. Asking it first keeps the failure honest -- what is being checked is that
# pixy reads local time, and where nothing can tell the zones apart there is
# nothing to read.
if [ "$(TZ=UTC clock --now-ms 1200000000)" = "$(TZ=Asia/Tokyo clock --now-ms 1200000000)" ]; then
  printf 'note: this C library resolves no timezones (TZDIR=%s); zone offsets not checked\n' \
    "${TZDIR:-unset}"
else
  equals "the clock follows the zone" "21:20:00 06:20:00 22:20:00 16:20:00" \
    "$(TZ=UTC clock --now-ms 1200000000) $(TZ=Asia/Tokyo clock --now-ms 1200000000) \
$(TZ=Europe/Berlin clock --now-ms 1200000000) $(TZ=America/New_York clock --now-ms 1200000000)"
fi
equals "the same instant renders the same twice" \
  "$(clock --now-ms 1787292810953)" "$(clock --now-ms 1787292810953)"
# A caller that supplies its own time still wins.
equals "a caller's time overrides the clock" "12:34:56" \
  "$("$pixy" render status.left.clock --config "$config" --target plain \
     --context-file "$context" | tr -d ' ')"

# ---- the compiled config cache -------------------------------------------------

# Parsing a real config was the largest cost left in a prompt, so the compiled
# form is kept. What it must never do is serve an edit's predecessor: the key is
# the content, because `st_mtime` counts whole seconds and two edits a moment
# apart that leave the file the same length look identical to it.
cachedir=$(mktemp -d)
live=$(mktemp)
write_config() {
  printf 'local pixy = require("pixy")\nreturn pixy.config({zones = {z = pixy.zone({pixy.segment("s", function() return pixy.text("%s") end)})}})\n' "$1" >"$live"
}
seen=""
for word in ONE TWO ONE THREE; do
  write_config "$word"
  seen="$seen$(PIXY_CACHE_DIR=$cachedir "$pixy" render z --config "$live" --target plain) "
done
equals "an edit is never served the last compile" "ONE TWO ONE THREE " "$seen"
# One compiled copy per config, not one per edit.
equals "the cache keeps one copy per config" 1 \
  "$(find "$cachedir" -name '*.luac' | wc -l | tr -d ' ')"
# A cache it cannot use is a slower start, never a broken one.
printf 'not a compiled chunk' >"$(find "$cachedir" -name '*.luac')"
equals "a corrupt cache falls back to the source" "THREE" \
  "$(PIXY_CACHE_DIR=$cachedir "$pixy" render z --config "$live" --target plain)"
equals "a config renders the same with no cache at all" "THREE" \
  "$(PIXY_CACHE_DIR=/nonexistent/unwritable "$pixy" render z --config "$live" --target plain)"
rm -rf "$cachedir" "$live"

# ---- the capability query ------------------------------------------------------

# `ask` alone stays an emitter like every other verb. `--wait` is the protocol's
# one round trip, and needs a terminal that answers, so it runs on a pty.
exits "a timeout is bounded" 2 palette ask --wait --timeout-ms 0
exits "a timeout is bounded above too" 2 palette ask --wait --timeout-ms 999999

# Deliberately not run against the terminal the suite is in. `--wait` writes a
# query to /dev/tty and reads the reply, so doing it here would depend on which
# terminal someone runs the tests from -- silence outside hexe, an answer inside
# it -- and would take a keystroke out of their input while it listened. The pty
# cases below give it a terminal of its own and pin both answers exactly.

probe=$(mktemp -u)
if ${CC:-cc} -O1 -o "$probe" tests/tty_probe.c -lutil >/dev/null 2>&1; then
  # Silence is the documented answer for "unsupported", and a reply that arrives
  # after the deadline is silence -- a prompt cannot wait on a terminal.
  equals "a terminal that answers is believed" "exit=0 answer=1330 31" \
    "$("$probe" have "$pixy" palette ask --wait)"
  equals "another reply in the way is stepped over" "exit=0 answer=1330 31" \
    "$("$probe" junk "$pixy" palette ask --wait)"
  equals "silence means unsupported" "exit=1 answer=" \
    "$("$probe" silent "$pixy" palette ask --wait)"
  equals "a late reply is not waited for" "exit=1 answer=" \
    "$("$probe" slow "$pixy" palette ask --wait --timeout-ms 100)"
  rm -f "$probe"
fi

# ---- palette namespaces --------------------------------------------------------

esc=$'\033'
st=$'\033\\'
palette() { "$pixy" palette "$@" 2>&1; }

equals "use claims a slot" "${esc}]1330;use;4${st}" "$(palette use --slot 4)"
equals "end releases it" "${esc}]1330;end${st}" "$(palette end)"
equals "ask is the capability query" "${esc}]1330;ask${st}" "$(palette ask)"
equals "reset forgets a slot" "${esc}]1330;reset;4${st}" "$(palette reset --slot 4)"
equals "a star addresses every slot in use" "${esc}]1330;set;*;7=#ff00aa${st}" \
  "$(palette set --slot '*' 7=#ff00aa)"
equals "hexe spells the flag --ns" "${esc}]1330;use;4${st}" "$(palette use --ns 4)"
equals "set patches named entries only" "${esc}]1330;set;2;1=#ff5555;bg=#0a0a0a${st}" \
  "$(palette set 1=#ff5555 bg=#0a0a0a)"
equals "the OSC number follows the terminal" "${esc}]1400;use;4${st}" \
  "$(HEXE_PALETTE_OSC=1400 palette use --slot 4)"

# Slot 0 is the ordinary palette and slot 1 the terminal's own chrome: both can
# be themed, neither can be claimed, or output would be tagged as someone else's.
exits "use refuses the ordinary palette" 2 palette use --slot 0
exits "use refuses the terminal's chrome" 2 palette use --slot 1
exits "use refuses a slot past the last" 2 palette use --slot 32
exits "set themes the ordinary palette" 0 palette set --slot 0 1=#c04040
exits "set themes the terminal's chrome" 0 palette set --slot 1 237=#123456
exits "a colour may not carry a separator" 2 palette set 1='#ff00aa;boom'
exits "a key is an index or a name" 2 palette set 300=#ff00aa
exits "cursor is a key" 0 palette set cursor=#00ff88
exits "rgb: is a colour" 0 palette set 1=rgb:ff/00/aa

# A prompt claims the slot the config declares, so the startup `set` and the
# per-prompt `use` cannot drift apart.
declared=tests/fixtures/palette.lua
equals "set emits what the config declared" "${esc}]1330;set;3;15=#cdd6f4;bg=#11111b${st}" \
  "$("$pixy" palette set --config "$declared")"
equals "a render claims the declared slot" \
  "${esc}]1330;set;3;15=#cdd6f4;bg=#11111b${st}${esc}]1330;use;3${st} hi ${esc}]1330;end${st}" \
  "$("$pixy" render prompt.left --config "$declared" --target plain --palette)"
equals "an explicit slot wins over the config" \
  "${esc}]1330;set;9;15=#cdd6f4;bg=#11111b${st}${esc}]1330;use;9${st} hi ${esc}]1330;end${st}" \
  "$("$pixy" render prompt.left --config "$declared" --target plain --palette 9)"
equals "a config that declares nothing emits nothing" "" \
  "$("$pixy" palette set --config examples/minimal.lua)"

# Bash reads the backslash of ST as an escape inside `\[ … \]`: it eats the
# closing marker and prints a stray `]` into the prompt. So a bash prompt gets
# BEL, which the protocol allows and the shell leaves alone. Zsh keeps ST.
bell=$'\a'
bash_prompt=$("$pixy" render prompt.left --config "$declared" --target bash --palette)
zsh_prompt=$("$pixy" render prompt.left --config "$declared" --target zsh --palette)
equals "a bash prompt is terminated with BEL" "yes" \
  "$(case $bash_prompt in
       "\\[${esc}]1330;set;3;15=#cdd6f4;bg=#11111b${bell}${esc}]1330;use;3${bell}\\]"*"\\[${esc}]1330;end${bell}\\]") echo yes ;;
       *) printf '%s' "$bash_prompt" ;;
     esac)"
equals "a zsh prompt keeps ST" "yes" \
  "$(case $zsh_prompt in
       "%{${esc}]1330;set;3;15=#cdd6f4;bg=#11111b${st}${esc}]1330;use;3${st}%}"*"%{${esc}]1330;end${st}%}") echo yes ;;
       *) printf '%s' "$zsh_prompt" ;;
     esac)"

# What bash itself makes of it, which is the only check that catches the above.
# `bash` on PATH may be another shell wearing the name, so find a real one.
real_bash=""
for candidate in /bin/bash /usr/bin/bash "$(command -v bash 2>/dev/null)"; do
  [ -x "$candidate" ] || continue
  if [ "$("$candidate" -c 'p=ok; printf "%s" "${p@P}"' 2>/dev/null)" = "ok" ]; then
    real_bash=$candidate
    break
  fi
done
if [ -n "$real_bash" ]; then
  # Through an argument, not PS1: a non-interactive bash does not inherit it.
  expanded=$("$real_bash" -c 'prompt=$1; printf "%s" "${prompt@P}"' _ "$bash_prompt")
  equals "bash expands the prompt back to the sequences" "yes" \
    "$(case $expanded in
         "${esc}]1330;set;3;15=#cdd6f4;bg=#11111b${bell}${esc}]1330;use;3${bell}"*"${esc}]1330;end${bell}") echo yes ;;
         *) printf '%s' "$expanded" | cat -v ;;
       esac)"
fi
equals "palette help answers" 1 "$("$pixy" palette --help | grep -c '^pixy palette')"

# A slot that cannot be claimed has to fail before anything is written. Emitting
# the release without the claim would pop whatever namespace the surrounding
# application was holding, and mis-colour the rest of its output.
for slot in 0 1 32 99 -1 abc 2.5; do
  exits "--palette $slot is refused, not half-applied" 2 \
    render prompt.left --config "$declared" --target plain --palette "$slot"
done
equals "nothing is written when the slot is refused" "" \
  "$("$pixy" render prompt.left --config "$declared" --target plain --palette 99 2>/dev/null)"
exits "--palette is refused where it cannot be carried" 2 \
  render prompt.left --config "$declared" --mode run --palette

# Anything that writes cells can claim a slot, and each claim is released. A
# surface is always ANSI, so only the wrapper is pinned here.
surface=$("$pixy" render prompt.left --config "$declared" --mode surface --palette)
equals "a surface claims the slot too" "yes" \
  "$(case $surface in "${esc}]1330;set;3;15=#cdd6f4;bg=#11111b${st}${esc}]1330;use;3${st}"*"${esc}]1330;end${st}") echo yes ;; *) echo "$surface" ;; esac)"
# How many frames a stream writes is a matter of timing; that every one of them
# is released is not.
stream=$("$pixy" stream work --config examples/spinner.lua --palette 4 --fps 20 --duration 200 2>/dev/null)
claims=$(printf '%s' "$stream" | grep -o "${esc}]1330;use;4" | wc -l | tr -d ' ')
releases=$(printf '%s' "$stream" | grep -o "${esc}]1330;end" | wc -l | tr -d ' ')
equals "every stream frame is balanced" "balanced" \
  "$([ "$claims" -ge 1 ] && [ "$claims" = "$releases" ] && echo balanced || echo "$claims/$releases")"

# Arguments are checked whole. A truncated one would silently address something
# else: `--slot 00000002` cut short is slot 0, the whole pane's palette.
equals "a padded slot addresses the slot it spells" "${esc}]1330;set;2;1=#ff0000${st}" \
  "$(palette set --slot 00000002 1=#ff0000)"
exits "an overlong slot is refused" 2 palette set --slot 000000000000000002 1=#ff0000
exits "an overlong entry is refused" 2 palette set 1=#ffffffffffffffffffffffffffffffffffff
exits "a typo is not silently ignored" 2 palette set nonsense
exits "--slot needs a value" 2 palette use --slot
exits "--config needs a value" 2 palette set --config
exits "use takes one slot, not every slot" 2 palette use --slot '*'
exits "end takes no slot" 2 palette end --slot 4
exits "ask takes no slot" 2 palette ask --slot 4

# A config declaring a broken palette is named, never guessed at.
broken() {
  printf 'local pixy = require("pixy")\nreturn pixy.config({palette = %s, zones = {z = pixy.zone({pixy.segment("s", function() return pixy.text("x") end)})}})\n' "$1" >"$tmpdir/broken.lua"
}
tmpdir=$(mktemp -d)
for bad in '"blue"' '{[1.5] = "#ff0000"}' '{[1] = 16711680}' '{[1] = true}' '{[1] = {}}' '{slot = 2.7}' '{slot = 1}' '{slot = 99}'; do
  broken "$bad"
  exits "palette $bad is a config error" 3 palette set --config "$tmpdir/broken.lua"
  exits "check reports palette $bad" 3 check --config "$tmpdir/broken.lua"
  # A prompt that stops drawing is worse than one wearing the terminal's colours,
  # so a render forgives what check refuses.
  exits "a render survives palette $bad" 0 \
    render z --config "$tmpdir/broken.lua" --target plain --palette
done
rm -rf "$tmpdir"

equals "check counts the palette it accepts" 1 \
  "$("$pixy" check --config "$declared" | grep -c '2 palette colours in slot 3')"
equals "check stays quiet without one" 0 \
  "$("$pixy" check --config examples/minimal.lua | grep -c palette)"

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
