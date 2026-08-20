#!/usr/bin/env bash
set -euo pipefail

pixy=${PIXY_BIN:-build/pixy}
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

# Every integration claims a namespace, and needs no startup hook to do it: the
# colours ride along with the claim, which is the only thing that works in a
# shell whose configuration cannot run a command.
for integration in bash zsh fish oslo; do
  grep -q -- '--palette' "$tmp/$integration"
  if grep -q 'palette set' "$tmp/$integration"; then exit 1; fi
done
"$pixy" render prompt.left --config "$tmp/hexe-oslo.lua" --target plain --width 20 \
  --context-file tests/fixtures/contexts/hexe-oslo.json --now-ms 0 >"$tmp/hexe-oslo.prompt"
grep -qx ' bresilla  7  λ ' "$tmp/hexe-oslo.prompt"
for integration in bash zsh fish oslo; do
  if grep -Eqi 'hexe|multiplexer|exit-policy' "$tmp/$integration"; then exit 1; fi
done

"$pixy" render prompt.left --target bash --set cwd=/work/project --set status=7 --set duration_ms=42 --set jobs=2 --set language=rust --set vimode=insert --set git_branch=fixture >"$tmp/prompt.bash"
"$pixy" render prompt.left --target zsh --set cwd=/work/project --set status=7 --set duration_ms=42 --set jobs=2 --set language=rust --set vimode=insert --set git_branch=fixture >"$tmp/prompt.zsh"
"$pixy" render prompt.right --target ansi --set cwd=/work/project --set status=7 --set duration_ms=42 --set jobs=2 --set language=rust --set vimode=insert >"$tmp/prompt.fish"
grep -Fq '\[' "$tmp/prompt.bash"
grep -Fq '%{' "$tmp/prompt.zsh"
grep -q 'rust' "$tmp/prompt.fish"

# A prompt claims the slot and releases it, with the sequences marked invisible
# so the shell does not count them as printable width.
"$pixy" render prompt.left --target bash --palette --set cwd=/work/project >"$tmp/palette.bash"
"$pixy" render prompt.left --target zsh --palette --set cwd=/work/project >"$tmp/palette.zsh"
osc=$'\033]1330'
st=$'\033\\'
# Bash reads the backslash of ST as an escape inside `\[ … \]`, so its sequences
# end in BEL instead. Zsh passes ST through untouched.
grep -Fq "\\[${osc};use;2"$'\a'"\\]" "$tmp/palette.bash"
grep -Fq "\\[${osc};end"$'\a'"\\]" "$tmp/palette.bash"
grep -Fq "%{${osc};use;2${st}%}" "$tmp/palette.zsh"
grep -Fq "%{${osc};end${st}%}" "$tmp/palette.zsh"

# The invariant behind the markers: bash's own expansion must give back exactly
# the sequences, with no marker eaten and no stray byte left printing.
real_bash=$(for c in /bin/bash /usr/bin/bash; do [ -x "$c" ] && echo "$c" && break; done)
expanded=$("${real_bash:-bash}" -c 'prompt=$1; printf "%s" "${prompt@P}"' _ "$(cat "$tmp/palette.bash")")
case $expanded in
  "${osc};use;2"$'\a'*"${osc};end"$'\a') ;;
  *) printf 'bash mangles the prompt: %s\n' "$(printf '%s' "$expanded" | cat -v)" >&2; exit 1 ;;
esac

printf 'shell smoke ok\n'
