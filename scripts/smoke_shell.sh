#!/usr/bin/env bash
set -euo pipefail

pixy=${PIXY_BIN:-target/debug/pixy}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export PIXY_CONFIG="$PWD/lua/pixy/default.lua"
export PIXY_CACHE_DIR="$tmp/cache"

for integration in bash zsh fish oslo; do
  "$pixy" init "$integration" >"$tmp/$integration"
  cmp "$tmp/$integration" "tests/fixtures/golden/init.$integration"
done

"$pixy" init hexe-oslo >"$tmp/hexe-oslo.lua"
cmp "$tmp/hexe-oslo.lua" examples/hexe-oslo.lua
"$pixy" check --config "$tmp/hexe-oslo.lua" >/dev/null

grep -q -- '--target bash' "$tmp/bash"
grep -q -- '--target zsh' "$tmp/zsh"
grep -q -- '--target ansi' "$tmp/fish"
for integration in bash zsh fish; do
  grep -q '__pixy_preexec' "$tmp/$integration"
  grep -q -- 'duration_ms=' "$tmp/$integration"
  grep -q -- 'jobs=' "$tmp/$integration"
  grep -q -- 'language=' "$tmp/$integration"
  grep -q -- 'vimode=' "$tmp/$integration"
done
grep -q 'oslo.prompt.left' "$tmp/oslo"
grep -q 'oslo.prompt.right' "$tmp/oslo"
grep -q 'command = "pixy"' "$tmp/oslo"
grep -q '\$status' "$tmp/oslo"
grep -q '\$duration_ms' "$tmp/oslo"
grep -q '\$jobs' "$tmp/oslo"
grep -q '\$language' "$tmp/oslo"
grep -q '\$vimode' "$tmp/oslo"
grep -q 'timeout_ms = 10' "$tmp/oslo"
grep -q 'async = true' "$tmp/oslo"
"$pixy" render prompt.left --config "$tmp/hexe-oslo.lua" --target plain --width 20 \
  --context-file tests/fixtures/contexts/hexe-oslo.json --now-ms 0 >"$tmp/hexe-oslo.prompt"
grep -qx 'bresilla sudo 7 λ ' "$tmp/hexe-oslo.prompt"
for integration in bash zsh fish oslo; do
  if grep -Eqi 'hexe|multiplexer|exit-policy' "$tmp/$integration"; then exit 1; fi
done

"$pixy" render prompt.left --target bash --set cwd=/work/project --set status=7 --set duration_ms=42 --set jobs=2 --set language=rust --set vimode=insert --set git_branch=fixture >"$tmp/prompt.bash"
"$pixy" render prompt.left --target zsh --set cwd=/work/project --set status=7 --set duration_ms=42 --set jobs=2 --set language=rust --set vimode=insert --set git_branch=fixture >"$tmp/prompt.zsh"
"$pixy" render prompt.right --target ansi --set cwd=/work/project --set status=7 --set duration_ms=42 --set jobs=2 --set language=rust --set vimode=insert >"$tmp/prompt.fish"
grep -Fq '\[' "$tmp/prompt.bash"
grep -Fq '%{' "$tmp/prompt.zsh"
grep -q 'rust' "$tmp/prompt.fish"

printf 'shell smoke ok\n'
