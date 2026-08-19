#!/usr/bin/env bash
# Regenerates the frames in docs/images from live pixy output.
#
#   make docs-images
#
# Needs a rasterizer on PATH; `nix shell nixpkgs#resvg` provides one.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

pixy=${PIXY:-target/release/pixy}
config=${PIXY_DOCS_CONFIG:-examples/hexe-oslo.lua}
context=tests/fixtures/contexts/hexe-oslo.json
out=docs/images
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out"

render() {
  "$pixy" render "$1" --config "$config" --target ansi --width "$2" \
    --context-file "$context" --now-ms 0
}

frame() {
  local name=$1 title=$2 source=$3
  awk -f scripts/ansi_svg.awk -v title="$title" "$source" >"$tmp/$name.svg"
  resvg --zoom 2 "$tmp/$name.svg" "$out/$name.png"
  printf '  %s\n' "$out/$name.png"
}

# A prompt line: the left zone, a gap, the right zone flushed to the margin.
width=104
left=$(render prompt.left "$width")
right=$(render prompt.right "$width")
left_cells=$("$pixy" render prompt.left --config "$config" --target plain --width "$width" \
  --context-file "$context" --now-ms 0 | wc -L)
right_cells=$("$pixy" render prompt.right --config "$config" --target plain --width "$width" \
  --context-file "$context" --now-ms 0 | wc -L)
gap=$((width - left_cells - right_cells))
[ "$gap" -lt 1 ] && gap=1
printf '%s%*s%s\n' "$left" "$gap" "" "$right" >"$tmp/prompt.ansi"
frame prompt "prompt" "$tmp/prompt.ansi"

# The status bar, whose spacers stretch it to the requested width.
{ render status 104; echo; } >"$tmp/status.ansi"
frame status "status bar" "$tmp/status.ansi"

# The same zone at four widths: segments drop by priority as room runs out.
: >"$tmp/responsive.ansi"
for w in 104 72 48 28; do
  { render status "$w"; echo; } >>"$tmp/responsive.ansi"
done
frame responsive "one zone, four widths" "$tmp/responsive.ansi"

# A truecolor sprite, drawn into a bounded surface.
"$pixy" render overlay --config "$config" --mode surface --width 34 --height 16 \
  --context-json '{"values":{"sprite_name":"pikachu","sprite_position":"center"}}' --now-ms 0 >"$tmp/sprite.ansi"
frame sprite "surface" "$tmp/sprite.ansi"

bash scripts/docs_anim.sh docs/images/responsive.svg

printf 'docs images written\n'
