#!/usr/bin/env bash
# Random input at the palette surface, where a mistake is a terminal left wearing
# someone else's colours rather than a crash.
#
#   tests/fuzz.sh [binary] [argument-rounds] [config-rounds]
#
# Two invariants, one per surface:
#   * argv     — never crashes, and every byte on stdout is a well-formed OSC
#   * config   — the same, plus a render always survives whatever it declared,
#                because a prompt that stops drawing is worse than a plain one
set -uo pipefail
cd "$(dirname "$0")/.."

pixy=${1:-build/pixy}
argument_rounds=${2:-1000}
config_rounds=${3:-200}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A sequence is OSC <number> ; <payload> ST, and a colour may carry no separator.
well_formed=$'^(\033\\][0-9]+;[^\033]*\033\\\\)+$'
crashes=0
malformed=0

fails() {
  printf '%s' "$1"
  shift
  printf ' [%s]' "$@"
  printf '\n'
}

# ---- the command line --------------------------------------------------------

tokens=(set use end reset ask have bogus '' -- - --slot --ns --config
        0 1 2 31 32 -1 007 00000002 '*' '**' 2.5 '2;3' 99999999999999999999
        1=#ff0000 255=#00ff00 fg=#fff bg=rgb:ff/00/aa cursor=#00ff88
        '1=#ff0000;use;1' '1=' '=#ff0000' '=' '1==2' 300=#ff0000 -5=#ff0000
        'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=#ff0000' '1=#ffffffffffffffffffffffffff'
        $'1=#ff\n0000' $'1=#ff\t0000' '1=#ff 0000' '1=$(id)' '1=`id`'
        --config=/dev/null --slot=4 --ns=3)

for ((round = 0; round < argument_rounds; round++)); do
  args=()
  for ((i = 0; i < RANDOM % 5 + 1; i++)); do
    args+=("${tokens[RANDOM % ${#tokens[@]}]}")
  done

  out=$("$pixy" palette "${args[@]}" 2>/dev/null)
  rc=$?
  if [ "$rc" -ge 128 ]; then
    crashes=$((crashes + 1))
    fails "CRASH rc=$rc args:" "${args[@]}"
  elif [ -n "$out" ] && ! printf '%s' "$out" | grep -qE "$well_formed"; then
    malformed=$((malformed + 1))
    fails "MALFORMED args:" "${args[@]}"
  fi
done

# ---- the configuration -------------------------------------------------------

keys=('[1]' '[0]' '[255]' '[256]' '[-1]' '[1.5]' 'fg' 'bg' 'cursor' 'slot'
      'unknown' '["1"]' '["fg"]' '["a very long key indeed"]')
values=('"#ff0000"' '"ff0000"' '"rgb:ff/00/aa"' '"#GGGGGG"' '"#ff00"' '""'
        '"#ff0000;use;1"' '16711680' 'true' 'nil' '{}' 'function() end' '2' '31' '"3"')

for ((round = 0; round < config_rounds; round++)); do
  {
    printf 'local pixy = require("pixy")\npixy.palette({'
    for ((i = 0; i < RANDOM % 4 + 1; i++)); do
      printf ' %s = %s,' "${keys[RANDOM % ${#keys[@]}]}" "${values[RANDOM % ${#values[@]}]}"
    done
    printf '})\npixy.zone("z", {pixy.segment("s", function() return pixy.text("x") end)})\n'
  } >"$tmp/config.lua"

  out=$("$pixy" palette set --config "$tmp/config.lua" 2>/dev/null)
  rc=$?
  if [ "$rc" -ge 128 ]; then
    crashes=$((crashes + 1))
    printf 'CRASH rc=%d\n%s\n' "$rc" "$(cat "$tmp/config.lua")"
    continue
  fi
  if [ -n "$out" ] && ! printf '%s' "$out" | grep -qE "$well_formed"; then
    malformed=$((malformed + 1))
    printf 'MALFORMED\n%s\n' "$(cat "$tmp/config.lua")"
  fi

  "$pixy" render z --config "$tmp/config.lua" --target plain --palette >/dev/null 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    crashes=$((crashes + 1))
    printf 'RENDER REFUSED rc=%d\n%s\n' "$rc" "$(cat "$tmp/config.lua")"
  fi
done

printf 'fuzz ok: %d argument rounds, %d config rounds, %d crashes, %d malformed\n' \
  "$argument_rounds" "$config_rounds" "$crashes" "$malformed"
[ "$crashes" -eq 0 ] && [ "$malformed" -eq 0 ]
