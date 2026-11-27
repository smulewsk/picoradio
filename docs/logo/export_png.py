#!/usr/bin/env python3
"""Exports transparent PNGs of the logo SVGs (macOS: uses qlmanage + sips).

    python3 docs/logo/export_png.py

qlmanage always renders onto an opaque canvas, so each SVG is rendered twice,
on black and on white, and the transparency is recovered from the difference
(alpha = 1 - (white - black)). Output goes to docs/logo/png/.
"""
import os, re, struct, subprocess, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))

def read_png(path):
    """8-bit RGBA/RGB PNG -> (w, h, bytearray RGBA)."""
    d = open(path, 'rb').read(); i = 8; idat = b''; ihdr = None
    while i < len(d):
        l = struct.unpack('>I', d[i:i+4])[0]; t = d[i+4:i+8]; c = d[i+8:i+8+l]
        if t == b'IHDR': ihdr = c
        if t == b'IDAT': idat += c
        i += 12 + l
    w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', ihdr)
    assert bd == 8 and ct in (2, 6) and il == 0, (bd, ct, il)
    bpp = 4 if ct == 6 else 3
    raw = zlib.decompress(idat); stride = w * bpp
    rows = []; prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]; line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for x in range(bpp, stride): line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride): line[x] = (line[x] + ((line[x - bpp] if x >= bpp else 0) + prev[x]) // 2) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line); prev = line
    out = bytearray(w * h * 4)
    for y, line in enumerate(rows):
        for x in range(w):
            o = (y * w + x) * 4
            out[o:o + 3] = line[x * bpp:x * bpp + 3]
            out[o + 3] = line[x * bpp + 3] if bpp == 4 else 255
    return w, h, out

def write_png(path, w, h, rgba):
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def render(svg_path, size, bg):
    """Renders the SVG `size` px wide on an opaque background -> (w, h, rgba)."""
    svg = open(svg_path).read()
    vw, vh = map(int, re.search(r'viewBox="0 0 (\d+) (\d+)"', svg).groups())
    svg = re.sub(r'(<svg[^>]*>)', r'\1<rect x="0" y="0" width="%d" height="%d" fill="%s"/>' % (vw, vh, bg), svg, count=1)
    with tempfile.TemporaryDirectory() as t:
        src = os.path.join(t, 'x.svg')
        open(src, 'w').write(svg)
        subprocess.run(['qlmanage', '-t', '-s', str(size), '-o', t, src], check=True, capture_output=True)
        w, h, px = read_png(os.path.join(t, 'x.svg.png'))
    rows = round(w * vh / vw)                       # qlmanage pads to a square: keep the top
    return w, rows, px[:w * rows * 4]

def transparent(svg_path, size):
    w, h, k = render(svg_path, size, '#000000')
    _, _, wt = render(svg_path, size, '#ffffff')
    out = bytearray(w * h * 4)
    for i in range(w * h):
        o = i * 4
        a = 255 - max(0, min(255, round(sum(wt[o + c] - k[o + c] for c in range(3)) / 3)))
        out[o + 3] = a
        for c in range(3):
            out[o + c] = min(255, round(k[o + c] * 255 / a)) if a else 0
    return w, h, out

def main():
    dst = os.path.join(HERE, 'png')
    os.makedirs(dst, exist_ok=True)
    for name, size in (('icon', 1024), ('logo', 1024), ('logo-light', 1024)):
        w, h, px = transparent(os.path.join(HERE, name + '.svg'), size)
        out = os.path.join(dst, f'{name}-{size}.png')
        write_png(out, w, h, px)
        print(out, w, 'x', h)
    for s in (512, 180):
        subprocess.run(['sips', '-Z', str(s), os.path.join(dst, 'icon-1024.png'), '--out',
                        os.path.join(dst, f'icon-{s}.png')], check=True, capture_output=True)
        print(os.path.join(dst, f'icon-{s}.png'))

if __name__ == '__main__':
    main()
