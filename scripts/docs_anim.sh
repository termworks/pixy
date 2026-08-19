#!/usr/bin/env bash
# Builds the animated README frame: one zone, swept across terminal widths.
#
#   make docs-images
#
# Frames are stacked in one SVG and cycled with CSS, which animates on GitHub
# without asking anyone to install a video codec.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

pixy=${PIXY:-target/release/pixy}
config=${PIXY_DOCS_CONFIG:-examples/hexe-oslo.lua}
context=tests/fixtures/contexts/hexe-oslo.json
out=${1:-docs/images/responsive.svg}
cols=104
pad=18
bar=34
lh=21
cw=8.43
hold=6          # frames held at each end of the sweep
step=4

widths=()
for ((w = cols; w >= 28; w -= step)); do widths+=("$w"); done
for ((i = 0; i < hold; i++)); do widths+=(28); done
for ((w = 28 + step; w <= cols; w += step)); do widths+=("$w"); done
for ((i = 0; i < hold; i++)); do widths+=("$cols"); done

total=${#widths[@]}
duration=$(awk -v n="$total" 'BEGIN { printf "%.2f", n * 0.09 }')
width=$(awk -v c="$cols" -v cw="$cw" -v p="$pad" 'BEGIN { printf "%.0f", c * cw + p * 2 }')
height=$(awk -v lh="$lh" -v p="$pad" -v b="$bar" 'BEGIN { printf "%.0f", lh + p * 2 + b }')

{
  printf '<?xml version="1.0" encoding="UTF-8"?>\n'
  printf '<svg xmlns="http://www.w3.org/2000/svg" width="%s" height="%s" viewBox="0 0 %s %s" font-family="DejaVu Sans Mono, monospace" font-size="14">\n' \
    "$width" "$height" "$width" "$height"
  printf '<style>\n'
  printf '.frame { visibility: hidden; }\n'
  for ((i = 0; i < total; i++)); do
    a=$(awk -v i="$i" -v n="$total" 'BEGIN { printf "%.4f", i * 100 / n }')
    b=$(awk -v i="$i" -v n="$total" 'BEGIN { printf "%.4f", (i + 1) * 100 / n }')
    printf '#f%d { animation: f%d %ss infinite; }\n' "$i" "$i" "$duration"
    if [ "$i" -eq 0 ]; then
      printf '@keyframes f%d { 0%%,%s%% { visibility: visible; } %s%%,100%% { visibility: hidden; } }\n' "$i" "$b" "$b"
    else
      printf '@keyframes f%d { 0%%,%s%% { visibility: hidden; } %s%%,%s%% { visibility: visible; } %s%%,100%% { visibility: hidden; } }\n' \
        "$i" "$a" "$a" "$b" "$b"
    fi
  done
  printf '</style>\n'
  printf '<rect width="%s" height="%s" rx="10" fill="#181825"/>\n' "$width" "$height"
  printf '<circle cx="20" cy="17" r="6" fill="#f38ba8"/><circle cx="40" cy="17" r="6" fill="#f9e2af"/><circle cx="60" cy="17" r="6" fill="#a6e3a1"/>\n'
  printf '<text x="%.0f" y="22" fill="#6c7086" text-anchor="middle" font-size="12">one zone, every width</text>\n' \
    "$(awk -v w="$width" 'BEGIN { print w / 2 }')"

  for ((i = 0; i < total; i++)); do
    printf '<g class="frame" id="f%d">\n' "$i"
    "$pixy" render status --config "$config" --target ansi --width "${widths[$i]}" \
      --context-file "$context" --now-ms 0 |
      awk -f scripts/ansi_svg.awk -v bare=1 -v pad="$pad" -v lh="$lh" -v cw="$cw" -v title=" "
    printf '\n</g>\n'
  done
  printf '</svg>\n'
} >"$out"

printf '  %s (%d frames, %ss loop)\n' "$out" "$total" "$duration"
