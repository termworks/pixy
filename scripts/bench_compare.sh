#!/usr/bin/env bash
# pixy against starship, at the only thing that matters for a prompt: what one
# process costs from exec to bytes on the terminal.
#
#   make bench-compare
#
# Both come from the flake, so the comparison is against a pinned starship
# rather than whichever one is on the machine. Three scenarios, because a prompt
# is not one workload:
#
#   bare    one directory segment and nothing else -- runtime overhead alone
#   git     a real repository, the case a prompt actually runs in
#   plain   outside a repository, where the git work should disappear
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

pixy=${PIXY_BIN:-$root/build/pixy}
starship=${PIXY_BENCH_STARSHIP:-$(command -v starship || true)}
runs=${PIXY_BENCH_RUNS:-200}

if [ ! -x "$pixy" ]; then
  printf 'no pixy at %s; run make release-build\n' "$pixy" >&2
  exit 1
fi
if [ -z "$starship" ] || [ ! -x "$starship" ]; then
  printf 'no starship; enter the dev shell with `nix develop`\n' >&2
  exit 1
fi
if ! command -v hyperfine >/dev/null; then
  printf 'no hyperfine; enter the dev shell with `nix develop`\n' >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Starship reads the user's config unless told otherwise, and a comparison
# against someone's personal prompt measures their choices, not the tool.
cat >"$work/starship-bare.toml" <<'TOML'
format = "$directory"
add_newline = false
[directory]
truncation_length = 3
TOML
cat >"$work/starship-full.toml" <<'TOML'
add_newline = false
TOML

plain=$work/plain
mkdir -p "$plain"

# Neither tool should be measured while it is still warming the filesystem
# cache, and neither should pay for the other's leftovers.
export STARSHIP_CACHE=$work/starship-cache
export PIXY_CACHE_DIR=$work/pixy-cache

compare() {
  local label=$1 dir=$2 pixy_cmd=$3 starship_cmd=$4
  printf '\n== %s\n' "$label"
  hyperfine --shell=none --warmup 20 --runs "$runs" \
    --style basic --time-unit millisecond \
    --command-name "pixy    $label" "$pixy_cmd" \
    --command-name "starship $label" "$starship_cmd" \
    --export-json "$work/$label.json" 2>&1 | grep -vE '^\s*$'
}

cd "$root"
compare bare "$root" \
  "$pixy render prompt.left --config $root/examples/minimal.lua --target ansi --set cwd=$root --set status=0" \
  "env STARSHIP_CONFIG=$work/starship-bare.toml $starship prompt --status=0 --jobs=0 --cmd-duration=0"

compare git "$root" \
  "$pixy render prompt.left --config $root/examples/hexe-oslo.lua --target ansi --set status=0 --set jobs=0 --set duration_ms=0" \
  "env STARSHIP_CONFIG=$work/starship-full.toml $starship prompt --status=0 --jobs=0 --cmd-duration=0"

printf '\n'
