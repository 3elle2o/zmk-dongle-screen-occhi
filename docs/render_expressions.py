"""Render every eye expression from the firmware's own geometry.

Ports the point generators out of eyes_status.c verbatim - same integer
division, same point order, same fill-and-stroke split - so what comes out is
what the code builds, not an impression of it.

Trig is float here where the firmware uses LVGL's lookup table; the difference
is well under a pixel. Everything else is the same arithmetic.

These are shapes, not screenshots. SVG anti-aliases differently from a canvas
fill under a rounded stroke, so use these to check a silhouette and the panel
to check how it looks.

    python docs/render_expressions.py

Writes one SVG per expression into docs/_preview/ for vetting - that directory
is ignored - and the contact sheet to docs/images/expressions.svg, which the
README uses.
"""
import math
import os

# --- constants, from the top of eyes_status.c ---
EYE_W, EYE_H, EYE_R, EYE_DX = 56, 76, 24, 46
LINE_W = 14
OPEN_FULL = 256
TRIG_MAX = 32767
STRAIN_MIN = 216
SPIRAL_PTS, SPIRAL_TURNS = 34, 900
ANGRY_CUT_OUTER_PCT, ANGRY_CUT_INNER_PCT = 42, 78
LID_TAIL = 14
SPARK_PTS, SPARK_FILL_PCT = 33, 100
ARC_PTS = 7
CROP_TOP_PCT = 14
RR_CORNER_PTS = 5
QUIRK_UP_PCT, QUIRK_SMALL_PCT = 116, 62
QUIRK_OUTLINE_W = 8
QUIRK_SMALL_D = EYE_W * QUIRK_SMALL_PCT // 100
SQUINT_TO = 110
LV_RADIUS_CIRCLE = -1  # stands in for LVGL's sentinel

OPENNESS = OPEN_FULL  # fully open; a blink is this shape at a lower value
STRAIN = OPEN_FULL

sin_of = lambda d: int(TRIG_MAX * math.sin(math.radians(d)))
cos_of = lambda d: int(TRIG_MAX * math.cos(math.radians(d)))
scaled = lambda v, o: (v * o) // OPEN_FULL


def rounded_rect(x0, y0, w, h, r):
    r = min(r, w // 2, h // 2)
    cx = [x0 + r, x0 + w - r, x0 + w - r, x0 + r]
    cy = [y0 + r, y0 + r, y0 + h - r, y0 + h - r]
    a0 = [180, 270, 0, 90]
    p = []
    for k in range(4):
        for i in range(RR_CORNER_PTS):
            deg = a0[k] + (90 * i) // (RR_CORNER_PTS - 1)
            p.append((cx[k] + (r * cos_of(deg)) // TRIG_MAX,
                      cy[k] + (r * sin_of(deg)) // TRIG_MAX))
    return p


def clip_below(src, x0, w, ya, yb):
    dst, n = [], len(src)
    for i in range(n):
        xi, yi = src[i]
        xj, yj = src[(i + 1) % n]
        di = yi - (ya + ((yb - ya) * (xi - x0)) // w)
        dj = yj - (ya + ((yb - ya) * (xj - x0)) // w)
        if di >= 0:
            dst.append((xi, yi))
        if (di >= 0) != (dj >= 0):
            den = di - dj or 1
            dst.append((xi + ((xj - xi) * di) // den, yi + ((yj - yi) * di) // den))
    return dst


def chevron(eye, w, box_h, inset):
    h = scaled(box_h, OPENNESS)
    k = STRAIN_MIN + ((OPEN_FULL - STRAIN_MIN) * STRAIN) // OPEN_FULL
    h = (h * k) // OPEN_FULL
    top = (box_h - h) // 2
    point_right = eye == 0
    apex = (w - inset) if point_right else inset
    open_ = inset if point_right else (w - inset)
    return [(open_, top + inset), (apex, top + h // 2), (open_, top + h - inset)]


def arc_down(w, box_h, inset):
    h = scaled(box_h, OPENNESS)
    top = (box_h - h) // 2
    span, depth = w - 2 * inset, h - 2 * inset
    p = []
    for i in range(ARC_PTS):
        deg = (180 * i) // (ARC_PTS - 1)
        p.append((inset + (span * i) // (ARC_PTS - 1),
                  top + inset + (depth * sin_of(deg)) // TRIG_MAX))
    return p


def lidded(w, box_h, inset):
    h = scaled(box_h, OPENNESS)
    top = (box_h - h) // 2
    eye_w = w - LID_TAIL - 2 * inset
    x0 = inset
    bowl_h = h - 2 * inset
    rr = rounded_rect(x0, top + inset - bowl_h, eye_w, 2 * bowl_h, EYE_R)
    half = clip_below(rr, x0, eye_w, top + inset, top + inset)
    p = [(x0 + eye_w + LID_TAIL, top + inset)] + half
    p.append(p[0])
    return p


def angry(eye, w, box_h, inset):
    h = scaled(box_h, OPENNESS)
    top = (box_h - h) // 2 + inset
    eh, x0, ew = h - 2 * inset, inset, w - 2 * inset
    rr = rounded_rect(x0, top, ew, eh, EYE_R)
    cut_out = top + (eh * ANGRY_CUT_OUTER_PCT) // 100
    cut_in = top + (eh * ANGRY_CUT_INNER_PCT) // 100
    ya, yb = (cut_in, cut_out) if eye else (cut_out, cut_in)
    p = clip_below(rr, x0, ew, ya, yb)
    p.append(p[0])
    return p


def cropped(w, box_h, inset):
    h = scaled(box_h, OPENNESS)
    top = (box_h - h) // 2 + inset
    eh, x0, ew = h - 2 * inset, inset, w - 2 * inset
    cut = top + (eh * CROP_TOP_PCT) // 100
    p = clip_below(rounded_rect(x0, top, ew, eh, EYE_R), x0, ew, cut, cut)
    p.append(p[0])
    return p


def spiral(w, h, inset):
    cx, cy = w // 2, h // 2
    r_max = scaled(min(w, h) // 2 - inset, OPENNESS)
    last = SPIRAL_PTS - 1
    p = []
    for i in range(SPIRAL_PTS):
        deg = (SPIRAL_TURNS * i) // last
        r = (r_max * i) // last
        p.append((cx + (r * cos_of(deg)) // TRIG_MAX, cy + (r * sin_of(deg)) // TRIG_MAX))
    return p


def twinkle(w, box_h, inset):
    """Outer contour then the sparkle, as one array - even-odd makes the hole."""
    h = scaled(box_h, OPENNESS)
    top = (box_h - h) // 2 + inset
    eh, ew = h - 2 * inset, w - 2 * inset
    cx, cy = inset + ew // 2, top + eh // 2
    tip_x = (ew // 2 - inset) * SPARK_FILL_PCT // 100
    tip_y = (eh // 2 - inset) * SPARK_FILL_PCT // 100

    p = rounded_rect(inset, top, ew, eh, EYE_R)
    p.append(p[0])
    outer = len(p)

    for i in range(SPARK_PTS):
        deg = (360 * (i % (SPARK_PTS - 1))) // (SPARK_PTS - 1)
        co, si = cos_of(deg), sin_of(deg)
        sn = max((abs(co) + abs(si)) // 256, 1)
        rhombus = (128 * TRIG_MAX) // sn
        concave = (128 * 128 * TRIG_MAX) // (sn * sn)
        r = (rhombus + concave) // 2
        rx, ry = (r * tip_x) // TRIG_MAX, (r * tip_y) // TRIG_MAX
        p.append((cx + (rx * cos_of(deg)) // TRIG_MAX, cy + (ry * sin_of(deg)) // TRIG_MAX))
    return p, outer


# shape, w, h, dx, dy, radius, line_w, spread, filled, outline_w
EXPR = {
    "neutral":        ("bar", EYE_W, EYE_H, 0, 0, EYE_R, 0, 0, False, 0),
    "squeezed":       ("chevron", EYE_W, EYE_H, 0, 0, 0, 0, 0, False, 0),
    "shock":          ("bar", 24, 24, 0, 0, 12, 0, 0, False, 0),
    "sleepy":         ("arc", EYE_W, 30, 0, 12, 0, 0, 0, False, 0),
    "unamused":       ("lidded", EYE_W + LID_TAIL, EYE_H // 2, 0, 0, 0, 9, 6, True, 0),
    "angry":          ("angry", EYE_W, EYE_H, 0, -14, 0, 9, 0, True, 0),
    "confused":       ("spiral", 86, 86, 0, 0, 0, 8, 6, False, 0),
    "neutral_up":     ("bar", EYE_W * QUIRK_UP_PCT // 100, EYE_H * QUIRK_UP_PCT // 100,
                       0, -7, EYE_R * QUIRK_UP_PCT // 100, 0, 0, False, QUIRK_OUTLINE_W),
    "neutral_down":   ("cropped", EYE_W, EYE_H, 0, 7, 0, QUIRK_OUTLINE_W, 0, False, 0),
    "neutral_small":  ("bar", QUIRK_SMALL_D, QUIRK_SMALL_D, 0, 0, LV_RADIUS_CIRCLE,
                       0, 0, False, QUIRK_OUTLINE_W),
    "neutral_twinkle": ("twinkle", EYE_W, EYE_H, 0, 0, 0, 9, 0, True, 0),
    "neutral_squint": ("bar", EYE_W, EYE_H * SQUINT_TO // OPEN_FULL, 0, 0, EYE_R,
                       0, 0, False, QUIRK_OUTLINE_W),
}

PANEL_W, PANEL_H = 230, 150
CX, CY = PANEL_W // 2, PANEL_H // 2


def draw_eye(out, name, eye):
    shape, w, h, dx0, dy, radius, line_w, spread, filled, outline_w = EXPR[name]
    lw = line_w or LINE_W
    inset = lw // 2
    out_x = EYE_DX + spread
    dx = (-out_x if eye == 0 else out_x) + dx0
    cx, cy = CX + dx, CY + dy

    if shape == "bar":
        bh = scaled(h, OPENNESS)
        r = min(w, bh) // 2 if radius == LV_RADIUS_CIRCLE else radius
        if outline_w:
            ow = outline_w
            out.append(f'<rect x="{cx-w//2+ow//2}" y="{cy-bh//2+ow//2}" width="{w-ow}" '
                       f'height="{bh-ow}" rx="{max(r-ow//2,0)}" fill="none" stroke="#fff" '
                       f'stroke-width="{ow}"/>')
        else:
            out.append(f'<rect x="{cx-w//2}" y="{cy-bh//2}" width="{w}" height="{bh}" '
                       f'rx="{r}" fill="#fff"/>')
        return

    box_h = h
    if shape == "chevron":
        pts, outer = chevron(eye, w, box_h, inset), None
    elif shape == "arc":
        pts, outer = arc_down(w, box_h, inset), None
    elif shape == "lidded":
        pts, outer = lidded(w, box_h, inset), None
    elif shape == "angry":
        pts, outer = angry(eye, w, box_h, inset), None
    elif shape == "cropped":
        pts, outer = cropped(w, box_h, inset), None
    elif shape == "spiral":
        pts, outer = spiral(w, h, inset), None
    elif shape == "twinkle":
        pts, outer = twinkle(w, box_h, inset)

    # Box coords -> panel coords: lv_obj_align centres the box on (cx, cy).
    m = [(cx - w // 2 + px, cy - box_h // 2 + py) for px, py in pts]
    d = " ".join(f"{x},{y}" for x, y in m)

    if filled:
        # One path, so even-odd can see both contours - the twinkle's hole
        # depends on it. Fill first, stroke over the top, as the canvas and
        # line objects do.
        out.append(f'<polygon points="{d}" fill="#fff" fill-rule="evenodd"/>')
        if outer:
            stroke = " ".join(f"{x},{y}" for x, y in m[:outer])
            out.append(f'<polyline points="{stroke}" fill="none" stroke="#fff" '
                       f'stroke-width="{lw}" stroke-linejoin="round" stroke-linecap="round"/>')
            hole = " ".join(f"{x},{y}" for x, y in m[outer:])
            out.append(f'<polyline points="{hole}" fill="none" stroke="#000" '
                       f'stroke-width="2" stroke-linejoin="round"/>')
        else:
            out.append(f'<polyline points="{d}" fill="none" stroke="#fff" '
                       f'stroke-width="{lw}" stroke-linejoin="round" stroke-linecap="round"/>')
    else:
        out.append(f'<polyline points="{d}" fill="none" stroke="#fff" stroke-width="{lw}" '
                   f'stroke-linejoin="round" stroke-linecap="round"/>')


def panel(name):
    out = [f'<rect width="{PANEL_W}" height="{PANEL_H}" fill="#000"/>']
    for eye in (0, 1):
        draw_eye(out, name, eye)
    return out


here = os.path.dirname(os.path.abspath(__file__))
preview = os.path.join(here, "_preview")
os.makedirs(preview, exist_ok=True)
names = list(EXPR)

for name in names:
    body = "\n".join(panel(name))
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PANEL_W} {PANEL_H}" '
           f'width="{PANEL_W*2}">\n{body}\n</svg>\n')
    open(os.path.join(preview, f"{name}.svg"), "w").write(svg)

# Contact sheet, four across.
COLS, LABEL = 4, 26
rows = (len(names) + COLS - 1) // COLS
W, H = PANEL_W * COLS, (PANEL_H + LABEL) * rows
sheet = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}">',
         f'<rect width="{W}" height="{H}" fill="#111"/>']
for i, name in enumerate(names):
    ox, oy = (i % COLS) * PANEL_W, (i // COLS) * (PANEL_H + LABEL)
    sheet.append(f'<g transform="translate({ox},{oy})">')
    sheet.append(f'<rect x="3" y="3" width="{PANEL_W-6}" height="{PANEL_H-6}" fill="#000" '
                 f'stroke="#2a2a2a"/>')
    sheet += panel(name)[1:]
    sheet.append(f'<text x="{CX}" y="{PANEL_H+16}" fill="#ddd" font-family="monospace" '
                 f'font-size="13" text-anchor="middle">{name}</text>')
    sheet.append('</g>')
sheet.append('</svg>')
open(os.path.join(here, "images", "expressions.svg"), "w").write("\n".join(sheet))

print(f"wrote {len(names)} panels to {preview}")
print("wrote images/expressions.svg")
