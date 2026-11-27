#!/usr/bin/env python3
"""Draws docs/wiring.svg: how the modules connect to the Pico 2 W.

    python3 docs/make_wiring.py

The Pico's 40 pins are drawn in their real header order (USB at the top).
Signal wires are coloured per module; power and ground are net labels
(3V3, 5V, GND) instead of wires, to keep the drawing readable.
Pins come from the tables below: keep them in step with include/config.h.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'wiring.svg')

# Pico 2 W header: physical pin -> name
LEFT = ['GP0', 'GP1', 'GND', 'GP2', 'GP3', 'GP4', 'GP5', 'GND', 'GP6', 'GP7',
        'GP8', 'GP9', 'GND', 'GP10', 'GP11', 'GP12', 'GP13', 'GND', 'GP14', 'GP15']      # pins 1..20
RIGHT = ['VBUS', 'VSYS', 'GND', '3V3_EN', '3V3', 'ADC_VREF', 'GP28', 'GND', 'GP27', 'GP26',
         'RUN', 'GP22', 'GND', 'GP21', 'GP20', 'GP19', 'GP18', 'GND', 'GP17', 'GP16']   # pins 40..21

W, H = 1180, 1000
PX0, PX1 = 470, 710                  # Pico body
TOP, STEP = 120, 30                  # first pin row, row pitch

def left_y(pin):  return TOP + (pin - 1) * STEP           # pins 1..20
def right_y(pin): return TOP + (40 - pin) * STEP          # pins 40..21

def gp_pin(name):
    for i, n in enumerate(LEFT):
        if n == name: return ('L', i + 1)
    for i, n in enumerate(RIGHT):
        if n == name: return ('R', 40 - i)
    raise KeyError(name)

def pin_pos(name):
    side, pin = gp_pin(name)
    return (PX0, left_y(pin)) if side == 'L' else (PX1, right_y(pin))

# dark theme (like the web page)
BG, PANEL, LINE, FG, DIM = '#0d0f14', '#171a22', '#2a2f3c', '#e8ebf2', '#8a93a6'
COL = {'enc': '#ffb028', 'dac': '#3ce65a', 'tft': '#3ca8ff', 'mic': '#c58cff',
       '3V3': '#ff6b5e', '5V': '#ff4638', 'GND': '#b8bfcc'}

out = []
def add(s): out.append(s)

def text(x, y, s, size=13, anchor='start', weight='normal', fill=None, family='Helvetica, Arial, sans-serif'):
    s = s.replace('&', '&amp;').replace('<', '&lt;')
    fill = fill or FG
    add(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" text-anchor="{anchor}" '
        f'font-weight="{weight}" fill="{fill}" dominant-baseline="middle">{s}</text>')

def line(pts, col, w=2.6):
    d = 'M' + ' L'.join(f'{x},{y}' for x, y in pts)
    add(f'<path d="{d}" fill="none" stroke="{col}" stroke-width="{w}" stroke-linejoin="round" stroke-linecap="round"/>')

def dot(x, y, col, r=3.6):
    add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{col}"/>')

def net(x, y, label, anchor='start'):
    """power net label at a module pin"""
    col = COL['GND'] if label == 'GND' else COL['5V'] if label == '5V' else COL['3V3']
    wdt = 10 + 8 * len(label)
    bx = x if anchor == 'start' else x - wdt
    add(f'<rect x="{bx}" y="{y - 10}" width="{wdt}" height="20" rx="4" fill="{BG}" stroke="{col}" stroke-width="1.6"/>')
    text(bx + wdt / 2, y + 1, label, 11, 'middle', 'bold', col)

def module(x, y, w, h, title, sub, col):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="{PANEL}" stroke="{col}" stroke-width="2.4"/>')
    text(x + w / 2, y + 18, title, 15, 'middle', 'bold', col)
    if sub: text(x + w / 2, y + 36, sub, 11, 'middle', 'normal', DIM)

def mod_pin(x, y, label, side, col):
    """module pin: small square on the box edge plus its label inside the box"""
    add(f'<rect x="{x - 4}" y="{y - 4}" width="8" height="8" fill="{col}"/>')
    if side == 'R': text(x - 10, y + 1, label, 12, 'end', 'bold')
    elif side == 'L': text(x + 10, y + 1, label, 12, 'start', 'bold')
    else: text(x, y + 16, label, 12, 'middle', 'bold')

# ---------------------------------------------------------------- canvas
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
text(W / 2, 30, 'Pico 2 W Internet Radio: wiring', 22, 'middle', 'bold')
text(W / 2, 56, 'Pins as in include/config.h. Power and ground are labels: connect every 3V3 / 5V / GND label to that Pico pin.',
     12, 'middle', 'normal', DIM)

# ---------------------------------------------------------------- Pico
add(f'<rect x="{PX0}" y="{TOP - 40}" width="{PX1 - PX0}" height="{19 * STEP + 80}" rx="14" fill="#1f6f3a" stroke="#14502a" stroke-width="2"/>')
add(f'<rect x="{(PX0 + PX1) / 2 - 26}" y="{TOP - 52}" width="52" height="24" rx="4" fill="#c9c9c9" stroke="#888"/>')
text((PX0 + PX1) / 2, TOP - 40, 'USB', 10, 'middle', 'bold', '#333')
text((PX0 + PX1) / 2, TOP + 9 * STEP, 'Raspberry Pi', 16, 'middle', 'bold', '#fff')
text((PX0 + PX1) / 2, TOP + 9 * STEP + 22, 'Pico 2 W', 20, 'middle', 'bold', '#fff')
text((PX0 + PX1) / 2, TOP + 9 * STEP + 48, '(top view)', 11, 'middle', 'normal', '#cfe8d6')

def pico_pin(x, y, num, name, side):
    gnd = name == 'GND'
    pwr = name in ('VBUS', '3V3')
    fill = '#111' if gnd else '#e0473b' if pwr else '#f2c94c'
    add(f'<circle cx="{x}" cy="{y}" r="6" fill="{fill}" stroke="#d9dde6" stroke-width="1.2"/>')
    if side == 'L':
        text(x + 12, y + 1, name, 11, 'start', 'bold', '#fff')
        text(x - 12, y + 1, str(num), 10, 'end', 'normal', DIM)
    else:
        text(x - 12, y + 1, name, 11, 'end', 'bold', '#fff')
        text(x + 12, y + 1, str(num), 10, 'start', 'normal', DIM)

for i, n in enumerate(LEFT):  pico_pin(PX0, left_y(i + 1), i + 1, n, 'L')
for i, n in enumerate(RIGHT): pico_pin(PX1, right_y(40 - i), 40 - i, n, 'R')

# ---------------------------------------------------------------- encoders (left, top)
def encoder(y0, title, sub, pins):
    x0, w, h = 60, 210, 150
    module(x0, y0, w, h, title, sub, COL['enc'])
    rows = [('CLK / S1', pins[0]), ('DT / S2', pins[1]), ('SW / KEY', pins[2])]
    ex = x0 + w
    for k, (lab, gp) in enumerate(rows):
        py = y0 + 62 + k * 26
        mod_pin(ex, py, lab, 'R', COL['enc'])
        tx, ty = pin_pos(gp)
        mx = 380 - k * 22            # wires heading down: the top one gets the outer lane
        line([(ex, py), (mx, py), (mx, ty), (tx, ty)], COL['enc'])
        dot(tx, ty, COL['enc'])
    mod_pin(x0, y0 + 88, '+ / 5V', 'L', COL['enc']); net(x0 - 10, y0 + 88, '3V3', 'end')
    mod_pin(x0, y0 + 118, 'GND', 'L', COL['enc']); net(x0 - 10, y0 + 118, 'GND', 'end')

encoder(80, 'Encoder 1', 'source / mute / info', ['GP2', 'GP3', 'GP4'])
encoder(250, 'Encoder 2', 'volume / stations', ['GP5', 'GP6', 'GP7'])

# ---------------------------------------------------------------- DAC (left, middle)
dx, dy, dw, dh = 60, 430, 210, 175
module(dx, dy, dw, dh, 'PCM5102 DAC', 'I2S, line out', COL['dac'])
dac = [('LRCK', 'GP9'), ('DATA', 'GP10'), ('BCK', 'GP11')]
for k, (lab, gp) in enumerate(dac):
    py = dy + 58 + k * 26
    mod_pin(dx + dw, py, lab, 'R', COL['dac'])
    tx, ty = pin_pos(gp)
    mx = 330 + k * 16
    line([(dx + dw, py), (mx, py), (mx, ty), (tx, ty)], COL['dac'])
    dot(tx, ty, COL['dac'])
mod_pin(dx + dw, dy + 140, 'VCC', 'R', COL['dac']); net(dx + dw + 10, dy + 140, '5V')
mod_pin(dx, dy + 116, 'GND', 'L', COL['dac']); net(dx - 10, dy + 116, 'GND', 'end')
mod_pin(dx, dy + 146, 'SCK*', 'L', COL['dac']); net(dx - 10, dy + 146, 'GND', 'end')

# ---------------------------------------------------------------- microphone (right, middle)
mx0, my0, mw, mh = 900, 470, 210, 160
module(mx0, my0, mw, mh, 'INMP441 mic', 'I2S, for AI Gemini', COL['mic'])
mic = [('SD', 'GP20'), ('WS', 'GP19'), ('SCK', 'GP18')]
for k, (lab, gp) in enumerate(mic):
    py = my0 + 58 + k * 26
    mod_pin(mx0, py, lab, 'L', COL['mic'])
    tx, ty = pin_pos(gp)
    xm = 850 - k * 16
    line([(mx0, py), (xm, py), (xm, ty), (tx, ty)], COL['mic'])
    dot(tx, ty, COL['mic'])
mod_pin(mx0 + mw, my0 + 70, 'VDD', 'R', COL['mic']); net(mx0 + mw + 10, my0 + 70, '3V3')
mod_pin(mx0 + mw, my0 + 100, 'GND', 'R', COL['mic']); net(mx0 + mw + 10, my0 + 100, 'GND')
mod_pin(mx0 + mw, my0 + 130, 'L/R', 'R', COL['mic']); net(mx0 + mw + 10, my0 + 130, 'GND')

# ---------------------------------------------------------------- TFT (bottom)
tx0, ty0, tw, th = 300, 840, 600, 120
tft_col = COL['tft']
add(f'<rect x="{tx0}" y="{ty0}" width="{tw}" height="{th}" rx="10" fill="{PANEL}" stroke="{tft_col}" stroke-width="2.4"/>')
text(tx0 + tw / 2, ty0 + 62, 'ST7789 2.25" TFT 76×284', 15, 'middle', 'bold', COL['tft'])
text(tx0 + tw / 2, ty0 + 86, 'SPI driven by PIO: any pins work', 11, 'middle', 'normal', DIM)
px = [tx0 + 40 + k * 66 for k in range(8)]
# (module pin, Pico pin, lane x, horizontal y): lanes picked so no wires cross
routes = [('BL', 'GP12', 400, 790), ('CS', 'GP13', 415, 805), ('DC', 'GP14', 430, 815), ('RST', 'GP15', 445, 785),
          ('SCL', 'GP16', 740, 785), ('SDA', 'GP17', 760, 805)]
for k, (lab, gp, lx, hy) in enumerate(routes):
    x = px[k]
    mod_pin(x, ty0, lab, 'T', COL['tft'])
    tx, ty = pin_pos(gp)
    line([(tx, ty), (lx, ty), (lx, hy), (x, hy), (x, ty0)], COL['tft'])
    dot(tx, ty, COL['tft'])
for k, lab in enumerate(['VCC', 'GND']):
    x = px[6 + k]
    mod_pin(x, ty0, lab, 'T', COL['tft'])
    net(x - 18, ty0 - 24, '3V3' if lab == 'VCC' else 'GND')

# ---------------------------------------------------------------- power notes (right, top)
nx, ny = 830, 100
add(f'<rect x="{nx}" y="{ny}" width="320" height="250" rx="10" fill="{PANEL}" stroke="{LINE}"/>')
text(nx + 16, ny + 22, 'Power and notes', 14, 'start', 'bold')
notes = [
    ('5V', 'Pico pin 40 (VBUS), from a good USB'),
    ('', 'supply: 4.75-5.25 V (VSYS reads ~0.3 V less)'),
    ('3V3', 'Pico pin 36 (3V3 OUT)'),
    ('GND', 'any GND pin: 3, 8, 13, 18, 23, 28, 33, 38'),
    ('*', 'SCK: tie to GND (or its pad), if the DAC has it'),
    ('', 'XSMT must be high (un-muted): see README'),
    ('BL', 'some displays light up on LOW:'),
    ('', 'TFT_BL_ACTIVE_LOW in config.h'),
    ('', 'Keep SCL / SDA wires short, GND next to them'),
]
for k, (lab, s) in enumerate(notes):
    y = ny + 50 + k * 22
    if lab in ('5V', '3V3', 'GND'): net(nx + 16, y, lab)
    elif lab: text(nx + 30, y, lab, 12, 'middle', 'bold', COL['5V'] if lab != 'BL' else COL['tft'])
    text(nx + 66, y, s, 11.5, 'start', 'normal', FG)

# legend
lx, ly = 60, 650
add(f'<rect x="{lx}" y="{ly}" width="250" height="118" rx="10" fill="{PANEL}" stroke="{LINE}"/>')
for k, (key, lab) in enumerate([('enc', 'encoders (KY-040)'), ('dac', 'DAC (audio out)'),
                                ('tft', 'display'), ('mic', 'microphone')]):
    y = ly + 22 + k * 24
    line([(lx + 16, y), (lx + 52, y)], COL[key], 4)
    text(lx + 64, y + 1, lab, 12)

add('</svg>')
with open(OUT, 'w') as f:
    f.write('\n'.join(out) + '\n')
print(f'{OUT}: written')
