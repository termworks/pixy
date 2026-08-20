#!/usr/bin/env bash
# Diffs this build against a reference binary, which during the port from Rust
# is the Rust binary itself. Every rendering path, byte for byte.
#
#   tests/parity.sh <reference-binary> [candidate-binary]
set -uo pipefail

reference=${1:?usage: parity.sh <reference> [candidate]}
candidate=${2:-build/pixy}
config=examples/hexe-oslo.lua
context=tests/fixtures/contexts/hexe-oslo.json
pass=0
fail=0

compare() {
  local label=$1
  shift
  local left right
  left=$("$reference" "$@" 2>&1)
  right=$("$candidate" "$@" 2>&1)
  if [ "$left" = "$right" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'DIFF %s\n  reference: %s\n  candidate: %s\n' "$label" "$left" "$right"
  fi
}

# Every zone the bundled profile defines, at the widths where pruning bites.
zones=$("$reference" list --config "$config" | grep -v '^float\.\|^container\.\|^overlay')
for zone in $zones; do
  for width in 120 88 60 40 24; do
    compare "$zone@$width plain" render "$zone" --config "$config" --target plain \
      --width "$width" --context-file "$context" --now-ms 0
    compare "$zone@$width run" render "$zone" --config "$config" --mode run \
      --width "$width" --context-file "$context" --now-ms 0
  done
done

# The joined spelling an integration writes, which the space-separated sweep
# above would never have caught.
compare "prompt.left joined flags" render prompt.left --config="$config" --target=plain \
  --width=100 --context-file="$context" --now-ms=0
compare "prompt.right joined flags" render prompt.right --config="$config" --target=ansi \
  --width=100 --context-file="$context" --now-ms=0

# Targets escape a line differently; a prompt integration depends on each.
for target in plain ansi bash zsh; do
  compare "prompt.left/$target" render prompt.left --config "$config" --target "$target" \
    --width 100 --context-file "$context" --now-ms 0
done

# Animation is deterministic in now_ms, so the same instant must draw the same.
for now in 0 40 120 300 640 1000; do
  compare "spinner@$now" render status.left.spinner --config "$config" --mode run --width 20 \
    --context-file "$context" --now-ms "$now"
done

# Surfaces: sprites through the embedded pack, and a popup.
for name in pikachu snorlax eevee charizard mew; do
  compare "sprite/$name" render overlay --config "$config" --mode surface --width 30 --height 14 \
    --context-json "{\"values\":{\"sprite_name\":\"$name\",\"sprite_position\":\"center\"}}" --now-ms 0
  compare "sprite/$name shiny" render overlay --config "$config" --mode surface --width 30 \
    --height 14 --context-json "{\"values\":{\"sprite_name\":\"$name\",\"sprite_shiny\":true}}" --now-ms 0
done

# Every character the packs and presets can produce, measured the same way.
compare "width table" render w --config tests/fixtures/width.lua --target plain --width 200 \
  --context-file tests/fixtures/width-context.json

# Inventory, packs and the vocabulary a host names things from.
compare "list" list --config "$config"
compare "check" check --config "$config"
compare "names" names
compare "names unknown" names definitely-not-a-pack
compare "pack list" pack list
# The integrations gained palette namespaces, which the reference predates. Take
# the palette back out and the rest must still match it line for line, so the
# one intended difference stays the only one.
compare_without_palette() {
  local shell=$1 left right
  left=$("$reference" init "$shell" 2>&1)
  right=$("$candidate" init "$shell" 2>&1 | sed \
    -e 's/ --palette//' -e 's/, "--palette"//' \
    -e '/^command pixy palette set$/d' \
    -e '/^-- The colours the pixy config declares/,/^os.execute("pixy palette set")$/d')
  if [ "$left" = "$right" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'DIFF init %s beyond the palette\n  reference: %s\n  candidate: %s\n' "$shell" "$left" "$right"
  fi
}

for shell in bash zsh fish oslo; do
  compare_without_palette "$shell"
done
compare "init hexe-oslo" init hexe-oslo
# The version is expected to differ from an older reference; only its shape is
# part of the contract.
if ! "$candidate" --version | grep -qE '^pixy [0-9]+\.[0-9]+\.[0-9]+$'; then
  fail=$((fail + 1))
  printf 'DIFF version format: %s\n' "$("$candidate" --version)"
else
  pass=$((pass + 1))
fi

# Failure paths: the message and the exit code are part of the interface.
compare "unknown zone" render nope --config "$config"
compare "unknown command" frobnicate
compare "bad mode" render prompt.left --config "$config" --mode sideways
compare "missing config" render prompt.left --config /nonexistent.lua

for command in "render nope --config $config" "frobnicate" "names definitely-not-a-pack"; do
  # shellcheck disable=SC2086
  "$reference" $command >/dev/null 2>&1
  left=$?
  # shellcheck disable=SC2086
  "$candidate" $command >/dev/null 2>&1
  right=$?
  if [ "$left" = "$right" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'DIFF exit code for `%s`: reference %s, candidate %s\n' "$command" "$left" "$right"
  fi
done

printf '\n%d identical, %d differing\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
