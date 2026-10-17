#!/usr/bin/env python3
"""Render the imserial app icon: a DE-9 serial port (D-shell and 9 pins) on a
violet tile. Writes assets/icon/imserial.ico (16-256 px), imserial-256.png and
imserial.svg. No dependencies beyond the standard library.

The same geometry is drawn at runtime by render_app_icon() in src/main.c and
draw_app_logo() in src/ui/titlebar.c; keep the three in sync.

Usage: python3 tools/gen_app_icon.py [output-dir]   (default: assets/icon)
"""
import math, os, struct, sys, zlib

BRAND = (0x6D, 0x28, 0xD9)  # violet tile
# Geometry in a 64x64 design space.
TILE = (4.0, 4.0, 60.0, 60.0, 13.0)  # x0, y0, x1, y1, corner radius
SHELL = [(13.5, 21.5), (50.5, 21.5), (45.75, 43.5), (18.25, 43.5)]  # D-shell outline


# Small sizes (<= 24 px) use this pixel-grid drawing instead: a 16x16 layout
# ('#' = white) on a full-bleed tile, exact at 16 px and scaled above that.
SMALL_TILE = (0.0, 0.0, 64.0, 64.0, 12.0)
SMALL = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "..############..",
    "..#..........#..",
    "..#.##.##.##.#..",
    "...#........#...",
    "...#.##..##.#...",
    "....########....",
    "................",
    "................",
    "................",
    "................",
    "................",
]


def variant(n):
    """Shell half-stroke and pins (centres, radius) for the vector drawing."""
    pins = [(20, 29), (26, 29), (32, 29), (38, 29), (44, 29), (23, 36.5), (29, 36.5), (35, 36.5), (41, 36.5)]
    return (2.0, pins, 2.6) if n <= 40 else (1.6, pins, 2.4)


def sd_round_rect(px, py, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    qx = abs(px - cx) - ((x1 - x0) / 2 - r)
    qy = abs(py - cy) - ((y1 - y0) / 2 - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def sd_poly(px, py, v):
    d = (px - v[0][0]) ** 2 + (py - v[0][1]) ** 2
    s = 1.0
    j = len(v) - 1
    for i in range(len(v)):
        ex, ey = v[j][0] - v[i][0], v[j][1] - v[i][1]
        wx, wy = px - v[i][0], py - v[i][1]
        h = min(max((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0), 1.0)
        d = min(d, (wx - ex * h) ** 2 + (wy - ey * h) ** 2)
        c1, c2, c3 = py >= v[i][1], py < v[j][1], ex * wy > ey * wx
        if (c1 and c2 and c3) or (not c1 and not c2 and not c3):
            s = -s
        j = i
    return s * math.sqrt(d)


def render(n, ss=4):
    """RGBA rows for an n x n icon, ss x ss supersampled."""
    half, pins, pr = variant(n)
    small = n <= 24
    rows = []
    for y in range(n):
        row = bytearray()
        for x in range(n):
            tile = ink = 0
            for sy in range(ss):
                for sx in range(ss):
                    fx = (x + (sx + 0.5) / ss) * 64.0 / n
                    fy = (y + (sy + 0.5) / ss) * 64.0 / n
                    if sd_round_rect(fx, fy, *(SMALL_TILE if small else TILE)) > 0:
                        continue
                    tile += 1
                    if small:
                        # 16..23 px: the 16 px drawing 1:1, centred (stays sharp); 24: scaled
                        if n < 24:
                            off = (n - 16) // 2
                            cx, cy = int(x + sx / ss) - off, int(y + sy / ss) - off
                        else:
                            cx, cy = int(fx / 4), int(fy / 4)
                        ink += 0 <= cx < 16 and 0 <= cy < 16 and SMALL[cy][cx] == "#"
                    elif abs(sd_poly(fx, fy, SHELL)) <= half or any(
                        (fx - cx) ** 2 + (fy - cy) ** 2 <= pr * pr for cx, cy in pins
                    ):
                        ink += 1
            a = tile / (ss * ss)
            w = ink / tile if tile else 0.0
            rgb = [round(c + (255 - c) * w) for c in BRAND]
            row += bytes(rgb + [round(a * 255)])
        rows.append(bytes(row))
    return rows


def png(rows):
    n = len(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + r for r in rows)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def ico(images):
    """images: [(size, png_bytes)] -> .ico with PNG-compressed entries."""
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    body = b""
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    return out + body


def svg():
    x0, y0, x1, y1, r = TILE
    shell = " ".join("%g,%g" % p for p in SHELL)
    half, pins, pr = variant(64)
    dots = "\n".join('  <circle cx="%g" cy="%g" r="%g" fill="#fff"/>' % (x, y, pr) for x, y in pins)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="256" height="256">\n'
        '  <rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#%02X%02X%02X"/>\n'
        '  <polygon points="%s" fill="none" stroke="#fff" stroke-width="%g" stroke-linejoin="round"/>\n'
        "%s\n</svg>\n" % (x0, y0, x1 - x0, y1 - y0, r, *BRAND, shell, half * 2, dots)
    )


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets", "icon")
    os.makedirs(out, exist_ok=True)
    images = [(n, png(render(n))) for n in (16, 20, 24, 32, 40, 48, 64, 128, 256)]
    with open(os.path.join(out, "imserial.ico"), "wb") as f:
        f.write(ico(images))
    with open(os.path.join(out, "imserial-256.png"), "wb") as f:
        f.write(images[-1][1])
    with open(os.path.join(out, "imserial.svg"), "w", newline="\n") as f:
        f.write(svg())


if __name__ == "__main__":
    main()
