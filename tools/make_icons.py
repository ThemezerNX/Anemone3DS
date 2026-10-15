#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Themezer-3DS contributors
"""Makes the app's UI icons from Lucide (https://lucide.dev, ISC license), the icon set the Themezer website uses.

Reads the icons from an @iconify-json/lucide package (the Themezer webapp has one), draws each as white strokes on a
transparent PNG in gfx/icons/, and writes gfx/icons.t3s listing them. The app tints them. The PNGs are committed, so a
build doesn't need this script or the package.

Every icon is drawn at 16 pixels, the size the app shows them next to text, so they stay sharp on the console's
screens; the few shown large (on empty pages and in messages) are drawn at 48 pixels as well.

    python3 tools/make_icons.py [path/to/@iconify-json/lucide/icons.json]

The SVGs are drawn by a small renderer of their own (paths, circles, rects, lines...) at 8x and scaled down, which
gives the same 2-pixel round strokes as on the website.
"""

import json
import math
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "gfx" / "icons"
DEFAULT_SOURCE = pathlib.Path.home() / "dev/themezer/themezer-monorepo/node_modules/@iconify-json/lucide/icons.json"
SMALL = 16
LARGE = 48
SCALE = 8

# The app's name for an icon, and the Lucide icon it is. The website's choices where it has one: themes are a
# paintbrush, splashes an image with a play button, badge sets an award.
ICONS = {
    "theme": "paintbrush",
    "splash": "image-play",
    "badge": "award",
    "download": "download",
    "install": "hard-drive-download",
    "check": "check",
    "check_circle": "circle-check",
    "music": "music",
    "music_only": "audio-lines",
    "no_music": "volume-x",
    "shuffle": "shuffle",
    "trash": "trash-2",
    "eye": "eye",
    "search": "search",
    "search_empty": "search-x",
    "qr": "scan-qr-code",
    "settings": "settings",
    "info": "info",
    "refresh": "refresh-cw",
    "restore": "rotate-ccw",
    "sort": "arrow-down-up",
    "globe": "globe",
    "wifi": "wifi",
    "wifi_off": "wifi-off",
    "warning": "triangle-alert",
    "error": "circle-x",
    "update": "circle-arrow-up",
    "save": "save",
    "power": "power",
    "screens": "monitor",
    "screen_top": "monitor-up",
    "screen_bottom": "monitor-down",
    "square": "square",
    "square_check": "square-check-big",
    "battery": "battery",
    "battery_charging": "battery-charging",
    "images": "images",
}

# The icons also drawn large.
LARGE_ICONS = ["theme", "splash", "badge", "error", "warning", "search_empty", "wifi_off", "qr",
               "check_circle", "update", "info", "trash"]

NUMBER = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


class Canvas:
    """Lucide's 24-unit grid, drawn at size pixels."""

    def __init__(self, size):
        self.size = size
        self.scale = SCALE * size / 24
        self.image = Image.new("L", (size * SCALE, size * SCALE), 0)
        self.draw = ImageDraw.Draw(self.image)

    def stroke(self, points, width, closed):
        if not points:
            return
        pts = [(x * self.scale, y * self.scale) for x, y in points]
        if closed and pts[0] != pts[-1]:
            pts.append(pts[0])
        w = width * self.scale
        r = w / 2
        if len(pts) > 1:
            self.draw.line(pts, fill=255, width=max(1, round(w)))
        # round caps and joins
        for x, y in pts:
            self.draw.ellipse((x - r, y - r, x + r, y + r), fill=255)

    def fill(self, points):
        if len(points) >= 3:
            self.draw.polygon([(x * self.scale, y * self.scale) for x, y in points], fill=255)

    def save(self, path):
        alpha = self.image.resize((self.size, self.size), Image.LANCZOS)
        icon = Image.new("RGBA", (self.size, self.size), (255, 255, 255, 0))
        icon.putalpha(alpha)
        icon.save(path)


def tokens(d):
    """Commands and numbers of path data, arc flags split one digit at a time where they run together."""
    out = []
    i = 0
    while i < len(d):
        c = d[i]
        if c.isalpha():
            out.append(c)
            i += 1
        elif c in " ,\t\n":
            i += 1
        else:
            m = NUMBER.match(d, i)
            if not m:
                raise ValueError(f"bad path data at {d[i:i + 10]!r}")
            out.append(m.group())
            i = m.end()
    return out


def arc_points(x0, y0, rx, ry, rotation, large, sweep, x1, y1):
    """An SVG arc as points, by the endpoint-to-centre conversion of the SVG specification."""
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    phi = math.radians(rotation)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)
    dx, dy = (x0 - x1) / 2, (y0 - y1) / 2
    x1p = cos_phi * dx + sin_phi * dy
    y1p = -sin_phi * dx + cos_phi * dy
    rx, ry = abs(rx), abs(ry)
    lam = (x1p ** 2) / (rx ** 2) + (y1p ** 2) / (ry ** 2)
    if lam > 1:
        rx *= math.sqrt(lam)
        ry *= math.sqrt(lam)
    num = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    den = rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2
    coef = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        coef = -coef
    cxp = coef * rx * y1p / ry
    cyp = -coef * ry * x1p / rx
    cx = cos_phi * cxp - sin_phi * cyp + (x0 + x1) / 2
    cy = sin_phi * cxp + cos_phi * cyp + (y0 + y1) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi
    steps = max(4, int(abs(delta) / (math.pi / 32)))
    points = []
    for i in range(1, steps + 1):
        t = theta1 + delta * i / steps
        x = cos_phi * rx * math.cos(t) - sin_phi * ry * math.sin(t) + cx
        y = sin_phi * rx * math.cos(t) + cos_phi * ry * math.sin(t) + cy
        points.append((x, y))
    return points


def bezier(p0, p1, p2, p3, steps=24):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def path_subpaths(d):
    """Path data as a list of (points, closed)."""
    toks = tokens(d)
    subpaths = []
    points = []
    x = y = 0.0
    start = (0.0, 0.0)
    last_control = None
    command = None
    i = 0

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def flag():
        # a flag is one digit, which compact path data runs into what follows: "01.5" is 0, 1, .5
        nonlocal i
        t = toks[i]
        if len(t) > 1 and t[0] in "01":
            toks[i] = t[1:]
            return int(t[0])
        i += 1
        return int(float(t))

    while i < len(toks):
        if toks[i].isalpha():
            command = toks[i]
            i += 1
        rel = command.islower()
        c = command.upper()
        if c == "M":
            if points:
                subpaths.append((points, False))
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            start = (x, y)
            points = [(x, y)]
            command = "l" if rel else "L"  # further pairs are lines
            last_control = None
        elif c == "Z":
            if points:
                subpaths.append((points, True))
            points = []
            x, y = start
            last_control = None
        elif c in "LHV":
            if c == "L":
                nx, ny = num(), num()
                x, y = (x + nx, y + ny) if rel else (nx, ny)
            elif c == "H":
                nx = num()
                x = x + nx if rel else nx
            else:
                ny = num()
                y = y + ny if rel else ny
            if not points:
                points = [start]
            points.append((x, y))
            last_control = None
        elif c in "CS":
            if c == "C":
                c1 = (num(), num())
                if rel:
                    c1 = (x + c1[0], y + c1[1])
            else:
                c1 = (2 * x - last_control[0], 2 * y - last_control[1]) if last_control else (x, y)
            c2 = (num(), num())
            end = (num(), num())
            if rel:
                c2 = (x + c2[0], y + c2[1])
                end = (x + end[0], y + end[1])
            if not points:
                points = [(x, y)]
            points += bezier((x, y), c1, c2, end)
            last_control = c2
            x, y = end
        elif c in "QT":
            if c == "Q":
                q = (num(), num())
                if rel:
                    q = (x + q[0], y + q[1])
            else:
                q = (2 * x - last_control[0], 2 * y - last_control[1]) if last_control else (x, y)
            end = (num(), num())
            if rel:
                end = (x + end[0], y + end[1])
            c1 = (x + 2 / 3 * (q[0] - x), y + 2 / 3 * (q[1] - y))
            c2 = (end[0] + 2 / 3 * (q[0] - end[0]), end[1] + 2 / 3 * (q[1] - end[1]))
            if not points:
                points = [(x, y)]
            points += bezier((x, y), c1, c2, end)
            last_control = q
            x, y = end
        elif c == "A":
            rx, ry, rotation = num(), num(), num()
            large = flag()
            sweep = flag()
            ex, ey = num(), num()
            if rel:
                ex, ey = x + ex, y + ey
            if not points:
                points = [(x, y)]
            points += arc_points(x, y, rx, ry, rotation, large, sweep, ex, ey)
            x, y = ex, ey
            last_control = None
        else:
            raise ValueError(f"unknown path command {command}")
    if points:
        subpaths.append((points, False))
    return subpaths


def ellipse_points(cx, cy, rx, ry):
    steps = 96
    return [(cx + rx * math.cos(2 * math.pi * i / steps), cy + ry * math.sin(2 * math.pi * i / steps))
            for i in range(steps)]


def rounded_rect_points(x, y, w, h, rx, ry):
    if rx <= 0 and ry <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    rx = min(rx or ry, w / 2)
    ry = min(ry or rx, h / 2)
    pts = []
    for cx, cy, a0 in ((x + w - rx, y + ry, -90), (x + w - rx, y + h - ry, 0), (x + rx, y + h - ry, 90),
                       (x + rx, y + ry, 180)):
        for i in range(13):
            a = math.radians(a0 + 90 * i / 12)
            pts.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return pts


def render(body, size):
    canvas = Canvas(size)
    root = ET.fromstring(f'<svg xmlns="http://www.w3.org/2000/svg">{body}</svg>')

    def walk(node, inherited):
        style = dict(inherited)
        for key in ("fill", "stroke", "stroke-width"):
            if key in node.attrib:
                style[key] = node.attrib[key]
        tag = node.tag.split("}")[-1]
        a = node.attrib
        shapes = []  # (points, closed)
        if tag == "path":
            shapes = path_subpaths(a["d"])
        elif tag == "circle":
            shapes = [(ellipse_points(float(a["cx"]), float(a["cy"]), float(a["r"]), float(a["r"])), True)]
        elif tag == "ellipse":
            shapes = [(ellipse_points(float(a["cx"]), float(a["cy"]), float(a["rx"]), float(a["ry"])), True)]
        elif tag == "rect":
            shapes = [(rounded_rect_points(float(a.get("x", 0)), float(a.get("y", 0)), float(a["width"]),
                                           float(a["height"]), float(a.get("rx", 0)), float(a.get("ry", 0))), True)]
        elif tag == "line":
            shapes = [([(float(a["x1"]), float(a["y1"])), (float(a["x2"]), float(a["y2"]))], False)]
        elif tag in ("polyline", "polygon"):
            values = [float(v) for v in NUMBER.findall(a["points"])]
            shapes = [(list(zip(values[0::2], values[1::2])), tag == "polygon")]
        for points, closed in shapes:
            if style.get("fill", "black") not in ("none", "transparent"):
                canvas.fill(points)
            if style.get("stroke", "none") != "none":
                canvas.stroke(points, float(style.get("stroke-width", 1)), closed)
        for child in node:
            walk(child, style)

    walk(root, {"fill": "black"})
    return canvas


def main():
    source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE
    data = json.loads(source.read_text())
    icons = data["icons"]
    aliases = data.get("aliases", {})
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.png"):
        old.unlink()
    for name, lucide in ICONS.items():
        while lucide not in icons and lucide in aliases:
            lucide = aliases[lucide]["parent"]
        render(icons[lucide]["body"], SMALL).save(OUT / f"{name}.png")
        if name in LARGE_ICONS:
            render(icons[lucide]["body"], LARGE).save(OUT / f"{name}_{LARGE}.png")
    missing = [name for name in LARGE_ICONS if name not in ICONS]
    if missing:
        raise SystemExit(f"LARGE_ICONS names icons that aren't in ICONS: {missing}")
    lines = ["--atlas -f la8 -z auto"] + [f"icons/{name}.png" for name in ICONS]
    lines += [f"icons/{name}_{LARGE}.png" for name in LARGE_ICONS]
    (ROOT / "gfx" / "icons.t3s").write_text("\n".join(lines) + "\n")
    print(f"{len(ICONS)} icons ({len(LARGE_ICONS)} also large) from Lucide "
          f"{data.get('info', {}).get('version', '')} in {OUT}")


if __name__ == "__main__":
    main()
