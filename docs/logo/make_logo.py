#!/usr/bin/env python3
"""Generates the Pico Radio logo SVGs.

    python3 make_logo.py fonts/Inter-var.ttf --wght 800 --size 56 --tracking 2 --joined --symbol eq

The wordmark is converted to vector outlines (needs fontTools:
`pip install fonttools`), so the logo looks identical everywhere without the
font installed - also on the radio's offline web page.

Writes icon.svg, logo.svg (for dark backgrounds) and logo-light.svg.
"""
import sys, os, argparse
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.transformPen import TransformPen

AMBER, AMBER_LIGHT = "#ffb028", "#b45f00"
WHITE, INK = "#e8ebf2", "#11141b"

# ---------------- badge: rounded square + LED "mountain" ----------------

import math

def arc(cx, cy, r, a1, a2):
    """SVG arc path, angles in degrees (0 = right, 90 = down)."""
    x1, y1 = cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1))
    x2, y2 = cx + r * math.cos(math.radians(a2)), cy + r * math.sin(math.radians(a2))
    return f"M{x1:.1f} {y1:.1f} A{r} {r} 0 0 1 {x2:.1f} {y2:.1f}"

SIGNAL = ["#3ce65a", "#ffb028", "#ff4638"]      # inner -> outer, like the VU colours

def symbol_wifi():
    cx, cy, sw = 64, 87, 10                     # fan pointing up, centred in the badge
    out = [f'<circle cx="{cx}" cy="{cy}" r="7.5" fill="{SIGNAL[0]}"/>']
    for i, r in enumerate((20, 36, 52)):
        out.append(f'<path d="{arc(cx, cy, r, -135, -45)}" fill="none" stroke="{SIGNAL[i]}" stroke-width="{sw}" stroke-linecap="round"/>')
    return "".join(out)

def symbol_broadcast():
    cx, cy, sw = 64, 64, 8                      # ((•)) transmitter
    out = [f'<circle cx="{cx}" cy="{cy}" r="9" fill="{SIGNAL[1]}"/>']
    for i, r in enumerate((21, 35, 49)):
        for a in (0, 180):
            out.append(f'<path d="{arc(cx, cy, r, a - 38, a + 38)}" fill="none" stroke="{SIGNAL[i]}" stroke-width="{sw}" stroke-linecap="round"/>')
    return "".join(out)

def symbol_wifi_small(cx=100, cy=44, colour="#ffb028"):
    """Small wireless sign for a corner of the badge."""
    out = [f'<circle cx="{cx}" cy="{cy}" r="2.8" fill="{colour}"/>']
    for r, op in ((7, 1), (13, .8), (19, .6)):
        out.append(f'<path d="{arc(cx, cy, r, -135, -45)}" fill="none" stroke="{colour}" stroke-opacity="{op}" stroke-width="3.6" stroke-linecap="round"/>')
    return "".join(out)

SYMBOL = "eq-wifi"

def badge():
    heights = [2, 4, 6, 8, 6, 4, 2]           # tallest in the centre, like the VU meters
    seg_w, seg_h, gap_x, gap_y = 10, 6, 4, 3
    total_w = len(heights) * seg_w + (len(heights) - 1) * gap_x
    stack_h = max(heights) * seg_h + (max(heights) - 1) * gap_y
    x0, base = (128 - total_w) / 2, 64 + stack_h / 2      # centred in the badge
    colour = lambda r: "#3ce65a" if r < 4 else ("#ffb028" if r < 6 else "#ff4638")
    segs = []
    for c, h in enumerate(heights):
        x = x0 + c * (seg_w + gap_x)
        for r in range(h):
            y = base - (r + 1) * seg_h - r * gap_y
            segs.append(f'<rect x="{x:.0f}" y="{y}" width="{seg_w}" height="{seg_h}" rx="1.6" fill="{colour(r)}"/>')
    defs = ('<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="#20263a"/><stop offset="1" stop-color="#0d0f14"/></linearGradient>'
            '<radialGradient id="glow" cx=".5" cy=".85" r=".6"><stop offset="0" stop-color="#3ce65a" stop-opacity=".18"/>'
            '<stop offset="1" stop-color="#3ce65a" stop-opacity="0"/></radialGradient></defs>')
    body = ('<rect x="5" y="5" width="118" height="118" rx="28" fill="url(#bg)" stroke="#2f3546" stroke-width="2"/>'
            '<rect x="5" y="5" width="118" height="118" rx="28" fill="url(#glow)"/>'
            '<rect x="11" y="11" width="106" height="106" rx="23" fill="none" stroke="#ffffff" stroke-opacity=".06" stroke-width="1.5"/>'
            + ("".join(segs) if SYMBOL == "eq" else
               "".join(segs) + symbol_wifi_small() if SYMBOL == "eq-wifi" else
               symbol_wifi() if SYMBOL == "wifi" else symbol_broadcast()))
    return defs, body

# ---------------- wordmark as outlines ----------------

def load_font(path, wght):
    font = TTFont(path)
    if "fvar" in font and wght:
        from fontTools.varLib import instancer
        font = instancer.instantiateVariableFont(font, {"wght": wght})
    return font

def word_paths(font, words, size, tracking, wordgap=0):
    """words: [(text, fill)]. Returns (svg paths in font units->px, width, (ymin, ymax))."""
    gs = font.getGlyphSet()
    cmap = font.getBestCmap()
    upm = font["head"].unitsPerEm
    scale = size / upm
    hmtx = font["hmtx"]
    x = 0.0
    out, ymin, ymax = [], 1e9, -1e9
    for text, fill in words:
        d = []
        for ch in text:
            g = cmap.get(ord(ch))
            if g is None:
                continue
            adv = hmtx[g][0] * scale
            if ch != " ":
                pen = SVGPathPen(gs, ntos=lambda v: ("%.1f" % v).rstrip("0").rstrip("."))   # 0.1 px is plenty
                # font units -> px, y flipped
                gs[g].draw(TransformPen(pen, (scale, 0, 0, -scale, x, 0)))
                d.append(pen.getCommands())
                bp = BoundsPen(gs)
                gs[g].draw(bp)
                if bp.bounds:
                    ymin = min(ymin, -bp.bounds[3] * scale)
                    ymax = max(ymax, -bp.bounds[1] * scale)
            x += adv + tracking + (wordgap if ch == " " else 0)
        out.append((" ".join(d), fill))
    return out, x - tracking, (ymin, ymax)

def lockup(font, fills, size, tracking, wordgap, joined=False):
    defs, body = badge()
    paths, width, (ymin, ymax) = word_paths(font, [("PICO" if joined else "PICO ", fills[0]), ("RADIO", fills[1])], size, tracking, wordgap)
    ty = 64 - (ymin + ymax) / 2          # centre the letters on the badge
    tx = 146
    w = int(tx + width + 10)
    g = "".join(f'<path d="{d}" fill="{f}"/>' for d, f in paths)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} 128" width="{w}" height="128">'
            f'<title>Pico Radio</title>{defs}<g>{body}</g>'
            f'<g transform="translate({tx} {ty:.2f})">{g}</g></svg>\n')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("outdir", nargs="?", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--wght", type=float, default=None)
    ap.add_argument("--size", type=float, default=60)
    ap.add_argument("--tracking", type=float, default=3)
    ap.add_argument("--joined", action="store_true", help="write PICORADIO as one word")
    ap.add_argument("--wordgap", type=float, default=0, help="extra px between PICO and RADIO")
    ap.add_argument("--symbol", choices=("eq-wifi", "eq", "wifi", "broadcast"), default="eq",
                    help="inside the badge: LED equalizer + small wireless sign, equalizer only, wireless fan, broadcast ((o))")
    a = ap.parse_args()
    global SYMBOL
    SYMBOL = a.symbol
    font = load_font(a.font, a.wght)
    defs, body = badge()
    os.makedirs(a.outdir, exist_ok=True)
    open(os.path.join(a.outdir, "icon.svg"), "w").write(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">'
        f'<title>Pico Radio</title>{defs}<g>{body}</g></svg>\n')
    open(os.path.join(a.outdir, "logo.svg"), "w").write(lockup(font, (AMBER, WHITE), a.size, a.tracking, a.wordgap, a.joined))
    open(os.path.join(a.outdir, "logo-light.svg"), "w").write(lockup(font, (AMBER_LIGHT, INK), a.size, a.tracking, a.wordgap, a.joined))
    print("written to", a.outdir)

if __name__ == "__main__":
    main()
