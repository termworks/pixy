# Turns ANSI output into an SVG of the line itself — no window frame, no title.
#
#   pixy render prompt.left --target ansi | awk -f scripts/ansi_svg.awk >frame.svg
#
# Variables: cols, rows, pad, fs (font size), cw (cell width), lh (line height).

function hex2(n) { return sprintf("%02x", n) }

function palette(i,   r, g, b, n) {
    if (i < 16) return base[i]
    if (i > 231) { n = 8 + 10 * (i - 232); return "#" hex2(n) hex2(n) hex2(n) }
    i -= 16
    r = int(i / 36); g = int((i % 36) / 6); b = i % 6
    return "#" hex2(cube[r]) hex2(cube[g]) hex2(cube[b])
}

function escape(s) {
    gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s)
    return s
}

function reset() { fg = ""; bg = ""; bold = 0; dim = 0; italic = 0; under = 0 }

function sgr(params,   n, i, code) {
    n = split(params, p, ";")
    for (i = 1; i <= n; i++) {
        code = p[i] + 0
        if (p[i] == "") code = 0
        if (code == 0) reset()
        else if (code == 1) bold = 1
        else if (code == 2) dim = 1
        else if (code == 3) italic = 1
        else if (code == 4) under = 1
        else if (code == 38 && p[i+1] == "5") { fg = palette(p[i+2] + 0); i += 2 }
        else if (code == 48 && p[i+1] == "5") { bg = palette(p[i+2] + 0); i += 2 }
        else if (code == 38 && p[i+1] == "2") { fg = sprintf("#%s%s%s", hex2(p[i+2]), hex2(p[i+3]), hex2(p[i+4])); i += 4 }
        else if (code == 48 && p[i+1] == "2") { bg = sprintf("#%s%s%s", hex2(p[i+2]), hex2(p[i+3]), hex2(p[i+4])); i += 4 }
        else if (code >= 30 && code <= 37) fg = base[code - 30]
        else if (code >= 40 && code <= 47) bg = base[code - 40]
        else if (code >= 90 && code <= 97) fg = base[code - 90 + 8]
        else if (code >= 100 && code <= 107) bg = base[code - 100 + 8]
        else if (code == 39) fg = ""
        else if (code == 49) bg = ""
    }
}

# Half-block sprite art tiles as rectangles; drawn as glyphs it leaves seams.
function blocks(   i, ch, only) {
    only = 1
    for (i = 1; i <= length(run); i++) {
        ch = substr(run, i, 1)
        if (ch != "▀" && ch != "▄" && ch != "█" && ch != " ") { only = 0; break }
    }
    return only
}

function tile(   i, ch, x, y, half) {
    y = pad + line * lh
    half = lh / 2
    for (i = 1; i <= length(run); i++) {
        ch = substr(run, i, 1)
        x = pad + (col + i - 1) * cw
        if (bg != "") body = body sprintf("  <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"%s\"/>\n", x, y, cw, lh, bg)
        if (ch == "█") body = body sprintf("  <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"%s\"/>\n", x, y, cw, lh, fg)
        else if (ch == "▀") body = body sprintf("  <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"%s\"/>\n", x, y, cw, half, fg)
        else if (ch == "▄") body = body sprintf("  <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"%s\"/>\n", x, y + half, cw, half, fg)
    }
    col += length(run)
    if (col > maxcol) maxcol = col
    run = ""
}

function flush(   x, w, style) {
    if (run == "") return
    if (fg != "" && blocks()) { tile(); return }
    x = pad + col * cw
    w = length(run) * cw
    if (bg != "") body = body sprintf("  <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"%s\"/>\n", x, pad + line * lh, w, lh, bg)
    style = ""
    if (bold) style = style " font-weight=\"700\""
    if (italic) style = style " font-style=\"italic\""
    if (under) style = style " text-decoration=\"underline\""
    body = body sprintf("  <text x=\"%.2f\" y=\"%.2f\" fill=\"%s\" fill-opacity=\"%s\"%s xml:space=\"preserve\">%s</text>\n",
        x, pad + line * lh + lh * 0.74, (fg == "" ? fgdefault : fg), (dim ? "0.55" : "1"), style, escape(run))
    col += length(run)
    if (col > maxcol) maxcol = col
    run = ""
}

BEGIN {
    split("#1e1e2e #f38ba8 #a6e3a1 #f9e2af #89b4fa #cba6f7 #94e2d5 #bac2de" \
          " #585b70 #f38ba8 #a6e3a1 #f9e2af #89b4fa #cba6f7 #94e2d5 #a6adc8", base, " ")
    for (i = 0; i < 16; i++) base[i] = base[i + 1]
    split("0 95 135 175 215 255", cube, " ")
    for (i = 0; i < 6; i++) cube[i] = cube[i + 1]

    if (cw == "") cw = 8.43
    if (lh == "") lh = 22
    if (fs == "") fs = 14
    if (pad == "") pad = 10
    maxcol = 0
    fgdefault = "#cdd6f4"
    line = 0; col = 0; run = ""
    reset()
    body = ""
    ground = ""
}

{
    text = $0
    col = 0; run = ""
    reset()
    while (length(text) > 0) {
        i = index(text, "\033")
        if (i == 0) { run = run text; break }
        if (i > 1) { run = run substr(text, 1, i - 1) }
        text = substr(text, i)
        if (match(text, /^\033\[[0-9;]*m/)) {
            flush()
            sgr(substr(text, 3, RLENGTH - 3))
            text = substr(text, RLENGTH + 1)
        } else if (match(text, /^\033\[[0-9]*C/)) {
            flush()
            skip = substr(text, 3, RLENGTH - 3) + 0
            col += (skip == 0 ? 1 : skip)
            if (col > maxcol) maxcol = col
            text = substr(text, RLENGTH + 1)
        } else if (match(text, /^\033\[[0-9;?]*[A-Za-z]/)) {
            text = substr(text, RLENGTH + 1)
        } else {
            run = run substr(text, 1, 1)
            text = substr(text, 2)
        }
    }
    flush()
    # Each line gets a ground exactly as wide as the line. Lines of different
    # widths then read as separate lines instead of one ragged block.
    if (col > 0) {
        ground = ground sprintf("  <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"#181825\"/>\n",
            pad, pad + line * lh, col * cw, lh)
    }
    line++
}

END {
    if (rows == "") rows = line
    if (cols == "") cols = maxcol
    w = cols * cw + pad * 2
    h = rows * lh + pad * 2
    print "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"0 0 %.0f %.0f\" font-family=\"JetBrainsMono Nerd Font, DejaVu Sans Mono, monospace\" font-size=\"%d\">\n", w, h, w, h, fs
    if (fill != "") printf "  <rect width=\"%.0f\" height=\"%.0f\" rx=\"4\" fill=\"#181825\"/>\n", w, h
    else printf "%s", ground
    printf "%s", body
    print "</svg>"
}
