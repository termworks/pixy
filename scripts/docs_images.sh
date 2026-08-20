#!/usr/bin/env bash
# Regenerates every frame in docs/images from live pixy output.
#
#   make docs-images
#
# Needs a rasterizer on PATH; `nix shell nixpkgs#resvg` provides one. Set
# PIXY_DOCS_FONTS to a directory of Nerd Font files for the powerline glyphs.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

pixy=${PIXY:-build/pixy}
config=${PIXY_DOCS_CONFIG:-examples/hexe-oslo.lua}
context=tests/fixtures/contexts/hexe-oslo.json
fonts=${PIXY_DOCS_FONTS:-}
out=docs/images
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out" "$out/presets"

# Renders happen at one width so the presets show the same shell state, but each
# frame is cropped to its own content; the README fixes the display scale.
COLS=88
FRAME_COLS=""   # each frame is sized to its own content

raster() {
  if [ -n "$fonts" ]; then
    resvg --zoom 2 --use-fonts-dir "$fonts" "$1" "$2"
  else
    resvg --zoom 2 "$1" "$2"
  fi
}

frame() {
  local name=$1 source=$2 fill=${3:-0} cols=${4:-$FRAME_COLS}
  awk -f scripts/ansi_svg.awk -v cols="$cols" -v fill="$fill" "$source" >"$tmp/$(basename "$name").svg"
  raster "$tmp/$(basename "$name").svg" "$out/$name.png"
  printf '  %s\n' "$out/$name.png"
}

render() {
  "$pixy" render "$1" --config "$2" --target ansi --width "$COLS" "${@:3}"
}

# The demo values every preset shares, so they differ by design only.
demo=(--set "cwd=$HOME/dev/pixy" --set git_branch=main --set git_status=dirty
      --set language=rust --set hostname=tron --set time=09:24 --set jobs=2
      --set git_ahead=2 --set git_staged=1 --set git_unstaged=3)

# A prompt followed by a command someone typed, which is how a prompt is seen.
typed() {
  printf '%s\033[38;5;250m %s\033[0m\n' "$1" "$2"
}

# ---- the bundled profile ----------------------------------------------------

left=$(render prompt.left "$config" --context-file "$context" --now-ms 0)
right=$(render prompt.right "$config" --context-file "$context" --now-ms 0)
left_cells=$("$pixy" render prompt.left --config "$config" --target plain --width "$COLS" \
  --context-file "$context" --now-ms 0 | wc -L)
right_cells=$("$pixy" render prompt.right --config "$config" --target plain --width "$COLS" \
  --context-file "$context" --now-ms 0 | wc -L)
gap=$((COLS - left_cells - right_cells))
[ "$gap" -lt 1 ] && gap=1
printf '%s%*s%s\n' "$left" "$gap" "" "$right" >"$tmp/prompt.ansi"
frame prompt "$tmp/prompt.ansi"

{ render status "$config" --context-file "$context" --now-ms 0; echo; } >"$tmp/status.ansi"
frame status "$tmp/status.ansi"

# The same zone at four widths, each line on a ground of its own width so the
# staircase is the point rather than an accident.
: >"$tmp/responsive.ansi"
for w in 88 64 44 26; do
  { "$pixy" render status --config "$config" --target ansi --width "$w" \
      --context-file "$context" --now-ms 0; echo; } >>"$tmp/responsive.ansi"
done
frame responsive "$tmp/responsive.ansi" 0

# ---- the small examples -----------------------------------------------------

typed "$(render prompt.left examples/minimal.lua --set "cwd=$HOME/dev/pixy" --set status=7)" \
  "make test" >"$tmp/minimal.ansi"
frame minimal "$tmp/minimal.ansi"

typed "$(render prompt.right examples/git.lua --set git_branch=main --set git_status=dirty)" \
  "git commit" >"$tmp/git.ansi"
frame git "$tmp/git.ansi"

: >"$tmp/spinner.ansi"
for now in 0 160 320 480 640; do
  { "$pixy" render work --config examples/spinner.lua --target ansi --width "$COLS" \
      --now-ms "$now"; echo; } >>"$tmp/spinner.ansi"
done
frame spinner "$tmp/spinner.ansi"

"$pixy" render overlay --config "$config" --mode surface --width 34 --height 16 \
  --context-json '{"values":{"sprite_name":"pikachu","sprite_position":"center"}}' \
  --now-ms 0 >"$tmp/sprite.ansi"
frame sprite "$tmp/sprite.ansi"

# ---- the preset gallery -----------------------------------------------------

for preset in examples/presets/*.lua; do
  name=$(basename "$preset" .lua)
  typed "$(render prompt.left "$preset" "${demo[@]}")" "make release-build" \
    >"$tmp/preset-$name.ansi"
  frame "presets/$name" "$tmp/preset-$name.ansi"
done

# The README shows each frame at half its pixel width, which is its natural size
# at the 2x the rasterizer runs. Rewriting the attribute from the file itself
# keeps the two from drifting whenever the cell metrics change.
png_width() { od -An -tu4 -j16 -N4 --endian=big "$1" | tr -d ' '; }

grep -o 'src="docs/images/[^"]*"' README.md | cut -d'"' -f2 | sort -u | while read -r png; do
  [ -f "$png" ] || continue
  sed -i -E "s|(src=\"$png\"[^>]*width=\")[0-9]+(\")|\1$(( $(png_width "$png") / 2 ))\2|" README.md
done

printf 'docs images written\n'
