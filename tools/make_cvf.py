#!/usr/bin/env python3
"""Build a CHDK custom curve file (.CVF) from a Lightroom preset or curve points.

CVF format: 4 curves x 1024 little-endian uint16, no header (8192 bytes).
Each curve maps a raw sensor value (10 bit scale, 0-1023) to a new value.
The 4 curves are the 4 Bayer sites: curve0/1 = even row even/odd column,
curve2/3 = odd row. Which color sits where depends on the camera (--cfa).

CHDK applies the curve to linear raw data, before the camera's own gamma.
Lightroom curves are drawn on gamma-encoded values, so each raw value is
converted to gamma space, run through the curve, and converted back.

Examples:
  make_cvf.py preset.xmp -o MYLOOK.CVF
  make_cvf.py --points "0,0 64,48 192,215 255,255" -o SCURVE.CVF
"""
import argparse
import re
import struct
import sys

CURVE_SIZE = 1024
WHITE = 1023
BLACK_12BIT = 127          # G10 black level on the 12 bit scale
G10_CFA = 0x01000201       # byte0=row0col0 ... byte3=row1col1, 0=R 1=G 2=B
G10_WB_DAYLIGHT = (1.963, 1.0, 1.580)   # R G B gains, from the G10 D65 color matrix
# G10 balanced camera RGB -> linear sRGB: DNG ForwardMatrix (to XYZ D50), then XYZ D50 -> sRGB
G10_CAM_TO_SRGB = [[1.5155, -0.3702, -0.1449],
                   [-0.1356, 1.6589, -0.5234],
                   [0.0212, -0.4887, 1.4672]]
COLOR_NAMES = "RGB"


def parse_points(text):
    pts = []
    for pair in re.findall(r"(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)", text):
        pts.append((float(pair[0]), float(pair[1])))
    return pts


def read_xmp_curves(path):
    """Return {'master','R','G','B'} -> list of (x, y) points on a 0-255 scale."""
    with open(path, encoding="utf-8", errors="replace") as f:
        xmp = f.read()
    names = {
        "master": "ToneCurvePV2012",
        "R": "ToneCurvePV2012Red",
        "G": "ToneCurvePV2012Green",
        "B": "ToneCurvePV2012Blue",
    }
    curves = {}
    for key, tag in names.items():
        m = re.search(r"<crs:%s>(.*?)</crs:%s>" % (tag, tag), xmp, re.S)
        if m:
            pts = [parse_points(li)[0] for li in re.findall(r"<rdf:li>(.*?)</rdf:li>", m.group(1), re.S)
                   if parse_points(li)]
            if len(pts) >= 2:
                curves[key] = pts
    if not curves:
        sys.exit("No point tone curve (crs:ToneCurvePV2012*) found in %s" % path)
    return curves


def pchip(points):
    """Monotone cubic interpolation (like Lightroom's smooth curve). Returns f(x) on 0-1."""
    pts = sorted(points)
    xs = [p[0] / 255.0 for p in pts]
    ys = [p[1] / 255.0 for p in pts]
    n = len(xs)
    if n == 2:
        x0, x1, y0, y1 = xs[0], xs[1], ys[0], ys[1]
        return lambda x: y0 + (y1 - y0) * (min(max(x, x0), x1) - x0) / ((x1 - x0) or 1)
    h = [xs[i + 1] - xs[i] for i in range(n - 1)]
    d = [(ys[i + 1] - ys[i]) / h[i] for i in range(n - 1)]
    m = [0.0] * n
    m[0], m[-1] = d[0], d[-1]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] <= 0:
            m[i] = 0.0
        else:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])

    def f(x):
        if x <= xs[0]:
            return ys[0]
        if x >= xs[-1]:
            return ys[-1]
        i = max(k for k in range(n - 1) if xs[k] <= x)
        t = (x - xs[i]) / h[i]
        t2, t3 = t * t, t * t * t
        return ((2 * t3 - 3 * t2 + 1) * ys[i] + (t3 - 2 * t2 + t) * h[i] * m[i]
                + (-2 * t3 + 3 * t2) * ys[i + 1] + (t3 - t2) * h[i] * m[i + 1])
    return f


def inv3(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g)
    return [[(e*i - f*h)/det, (c*h - b*i)/det, (b*f - c*e)/det],
            [(f*g - d*i)/det, (a*i - c*g)/det, (c*d - a*f)/det],
            [(d*h - e*g)/det, (b*g - a*h)/det, (a*e - b*d)/det]]


def build_tables(funcs, gamma, black, gains, matrix=None):
    """Raw (linear, 10 bit) -> raw tables for R, G, B.

    funcs:  per channel curve f(x) on 0-1 gamma encoded values (the look, in sRGB).
    gains:  white balance multipliers (green = 1). Raw red and blue sit far
            below green until the camera balances them, so each value is scaled
            to its balanced brightness before the curve and back after.
    matrix: camera (balanced) -> linear sRGB matrix. The camera mixes channels
            through it after the curve, so the target look is pulled back through
            its inverse. Exact for neutral tones, close for colors.
    """
    minv = inv3(matrix) if matrix else [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    span = WHITE - black

    def cam_out(x):
        """balanced camera RGB that renders as the look's output for gray input x."""
        t = [min(max(funcs[c](x), 0.0), 1.0) ** gamma for c in "RGB"]
        return [max(sum(minv[r][k] * t[k] for k in range(3)), 0.0) for r in range(3)]

    top = cam_out(1.0)
    tables = {}
    for ci, c in enumerate("RGB"):
        table = []
        for i in range(CURVE_SIZE):
            if i <= black:
                table.append(i)
                continue
            lin = (i - black) / span * gains[c]
            if lin >= 1.0:
                out_lin = lin * top[ci]                 # beyond white: keep proportional
            else:
                out_lin = cam_out(lin ** (1.0 / gamma))[ci]
            out = black + out_lin / gains[c] * span
            table.append(int(round(min(max(out, 0), WHITE))))
        tables[c] = table
    return tables


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xmp", nargs="?", help="Lightroom preset (.xmp) with a point tone curve")
    ap.add_argument("--points", help='master curve points on 0-255, e.g. "0,0 64,48 192,215 255,255"')
    ap.add_argument("-o", "--out", required=True, help="output .CVF file")
    ap.add_argument("--gamma", type=float, default=2.2, help="gamma the curve was drawn in (default 2.2, 1 = raw linear)")
    ap.add_argument("--cfa", type=lambda s: int(s, 0), default=G10_CFA, help="camera CFA pattern (default G10)")
    ap.add_argument("--wb", default="%.3f,%.3f" % (G10_WB_DAYLIGHT[0], G10_WB_DAYLIGHT[2]),
                    help="red,blue white balance gains (default G10 daylight); 1,1 to disable")
    ap.add_argument("--no-matrix", action="store_true", help="ignore the camera color matrix (old behaviour)")
    ap.add_argument("--black", type=float, default=BLACK_12BIT / 4.0, help="black level on the 10 bit scale")
    args = ap.parse_args()

    if args.xmp:
        curves = read_xmp_curves(args.xmp)
    elif args.points:
        curves = {"master": parse_points(args.points)}
    else:
        ap.error("give an .xmp file or --points")

    master = pchip(curves["master"]) if "master" in curves else (lambda x: x)
    wb_r, wb_b = (float(v) for v in args.wb.split(","))
    gains = {"R": wb_r, "G": 1.0, "B": wb_b}
    funcs = {}
    for c in "RGB":
        ch = pchip(curves[c]) if c in curves else (lambda x: x)
        funcs[c] = lambda x, ch=ch: ch(master(x))
    per_color = build_tables(funcs, args.gamma, args.black, gains,
                             None if args.no_matrix else G10_CAM_TO_SRGB)

    # curve slot k (0..3) holds the color at CFA byte k
    slots = [COLOR_NAMES[(args.cfa >> (8 * k)) & 0xFF] for k in range(4)]
    data = b"".join(struct.pack("<%dH" % CURVE_SIZE, *per_color[c]) for c in slots)
    assert len(data) == 8192 and per_color[slots[0]][0] <= 1023

    with open(args.out, "wb") as f:
        f.write(data)
    print("Wrote %s (curves: %s, from: %s)" % (args.out, " ".join(slots), ", ".join(sorted(curves))))


if __name__ == "__main__":
    main()
