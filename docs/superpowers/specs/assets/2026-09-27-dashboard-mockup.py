# THROWAWAY design mockup: renders widget variants to 1-bit PNGs for the brainstorm.
# Not product code.
import csv, json, math, sys, calendar
from datetime import date, datetime, timedelta
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
OUT = Path(sys.argv[1])
TODAY = date(2026, 9, 27)
H = 464
SIZES = {"third": 266, "twothirds": 534, "full": 800}
FD = "/usr/share/fonts/truetype/dejavu/"


def font(sz, bold=False, cond=False):
    name = "DejaVuSans" + ("Condensed" if cond else "") + ("-Bold" if bold else "") + ".ttf"
    return ImageFont.truetype(FD + name, sz)


# ---------------- data ----------------
def load_fred(sid):
    rows = []
    with open(HERE / "data" / f"{sid}.csv") as f:
        r = csv.reader(f)
        next(r)
        for d, v in r:
            try:
                rows.append((date.fromisoformat(d), float(v)))
            except ValueError:
                pass
    return rows


def weekly(rows, start, end):
    """Last value of each week (Sunday-anchored), carried forward."""
    out, i, last = [], 0, None
    wk = start
    while wk <= end:
        while i < len(rows) and rows[i][0] <= wk:
            last = rows[i][1]
            i += 1
        if last is not None:
            out.append((wk, last))
        wk += timedelta(days=7)
    return out


START = TODAY - timedelta(days=365 * 5)
SERIES = [  # key, label, style, raw formatter
    ("SP500", "S&P 500", "thick", lambda v: f"{v:,.0f}"),
    ("CBBTCUSD", "Bitcoin", "dash", lambda v: f"${v/1000:.1f}k"),
    ("MORTGAGE30US", "Mortgage", "thin", lambda v: f"{v:.2f}%"),
    ("MEDLISPRI31080", "LA home", "dot", lambda v: f"${v/1e6:.2f}M"),
]
SHORT = {"S&P 500": "S&P", "Bitcoin": "BTC", "Mortgage": "Mort", "LA home": "Home"}
MKT = []
for key, label, style, fmt in SERIES:
    raw = load_fred(key)
    w = weekly(raw, START, TODAY)
    mean = sum(v for _, v in w) / len(w)
    yr_ago = next(v for d, v in w if d >= TODAY - timedelta(days=365))
    MKT.append(dict(label=label, style=style, pts=[(d, v / mean) for d, v in w],
                    last=w[-1][1], ratio=w[-1][1] / mean, yoy=w[-1][1] / yr_ago - 1,
                    fmt=fmt, asof=raw[-1][0]))

WX = json.load(open(HERE / "data" / "weather.json"))
WMO = {0: ("Clear", "sun"), 1: ("Mostly clear", "sun"), 2: ("Partly cloudy", "part"),
       3: ("Overcast", "cloud"), 45: ("Fog", "fog"), 48: ("Fog", "fog")}


def wmo(code):
    if code in WMO:
        return WMO[code]
    if 51 <= code <= 67 or 80 <= code <= 82:
        return ("Rain", "rain")
    if 71 <= code <= 77 or code in (85, 86):
        return ("Snow", "snow")
    return ("Storms", "storm")


# ---------------- drawing helpers ----------------
def styled_line(d, pts, style):
    if style == "thick":
        d.line(pts, fill=0, width=3, joint="curve")
        return
    if style == "thin":
        d.line(pts, fill=0, width=1)
        return
    on, off, wdt = (7, 4, 2) if style == "dash" else (2, 3, 2)
    acc, drawing = 0.0, True
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        t = 0.0
        while t < seg:
            lim = on if drawing else off
            step = min(lim - acc, seg - t)
            if drawing:
                a, b = t / seg, (t + step) / seg
                d.line([(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a),
                        (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)], fill=0, width=wdt)
            t += step
            acc += step
            if acc >= lim:
                acc, drawing = 0.0, not drawing


MARK = {"thick": "sq", "dash": "circ", "thin": "tri", "dot": "dia"}


def marker(d, x, y, kind, r=4):
    if kind == "sq":
        d.rectangle([x - r, y - r, x + r, y + r], fill=0)
    elif kind == "circ":
        d.ellipse([x - r - 1, y - r - 1, x + r + 1, y + r + 1], fill=255, outline=0, width=2)
    elif kind == "tri":
        d.polygon([(x, y - r - 1), (x + r + 1, y + r), (x - r - 1, y + r)], fill=0)
    else:
        d.polygon([(x, y - r - 2), (x + r + 2, y), (x, y + r + 2), (x - r - 2, y)], fill=255, outline=0, width=2)


def line_with_markers(d, pts, style, k, n):
    styled_line(d, pts, style)
    span = pts[-1][0] - pts[0][0]
    step = 56
    x = pts[0][0] + step * (k + 0.5) / n + 6
    j = 0
    while x < pts[-1][0] - 4:
        while pts[j + 1][0] < x:
            j += 1
        marker(d, x, pts[j][1], MARK[style])
        x += step
    marker(d, pts[-1][0], pts[-1][1], MARK[style])


def sample(d, x, y, style, length=26):
    styled_line(d, [(x, y), (x + length, y)], style)
    marker(d, x + length / 2, y, MARK[style])


def text_w(d, s, f):
    return d.textlength(s, font=f)


def icon(d, kind, cx, cy, r):
    """Simple geometric weather icons."""
    def sun(cx, cy, r):
        d.ellipse([cx - r * .45, cy - r * .45, cx + r * .45, cy + r * .45], outline=0, width=max(2, int(r * .12)))
        for k in range(8):
            a = k * math.pi / 4
            d.line([(cx + math.cos(a) * r * .65, cy + math.sin(a) * r * .65),
                    (cx + math.cos(a) * r * .95, cy + math.sin(a) * r * .95)], fill=0, width=max(2, int(r * .1)))

    def cloud(cx, cy, r, fill=255):
        w = max(2, int(r * .1))
        parts = [(cx - r * .45, cy + r * .1, r * .38), (cx, cy - r * .15, r * .5), (cx + r * .45, cy + r * .12, r * .36)]
        for x, y, rr in parts:
            d.ellipse([x - rr, y - rr, x + rr, y + rr], outline=0, width=w)
        for x, y, rr in parts:
            d.ellipse([x - rr + w, y - rr + w, x + rr - w, y + rr - w], fill=fill)
        d.rectangle([cx - r * .45, cy + r * .1, cx + r * .45, cy + r * .48 - w], fill=fill)
        d.line([(cx - r * .45, cy + r * .48), (cx + r * .45, cy + r * .48)], fill=0, width=w)

    if kind == "sun":
        sun(cx, cy, r)
    elif kind == "part":
        sun(cx - r * .3, cy - r * .3, r * .7)
        cloud(cx + r * .1, cy + r * .15, r * .8)
    elif kind == "cloud":
        cloud(cx, cy, r)
    elif kind in ("rain", "snow", "storm"):
        cloud(cx, cy - r * .2, r * .85)
        for k in range(3):
            x = cx - r * .35 + k * r * .35
            if kind == "snow":
                d.text((x, cy + r * .55), "*", font=font(int(r * .5), True), fill=0, anchor="mm")
            else:
                d.line([(x, cy + r * .35), (x - r * .12, cy + r * .75)], fill=0, width=max(2, int(r * .1)))
    elif kind == "fog":
        for k in range(4):
            y = cy - r * .4 + k * r * .27
            d.line([(cx - r * .7, y), (cx + r * .7, y)], fill=0, width=max(2, int(r * .1)))


# ---------------- Market trends ----------------
def market(variant, w):
    img = Image.new("L", (w, H), 255)
    d = ImageDraw.Draw(img)
    narrow = w < 400
    pad = 12
    d.text((pad, pad), "Markets", font=font(22 if not narrow else 18, True), fill=0)
    sub = "5-yr, × own average, log" if not narrow else "5-yr · ×avg · log"
    d.text((pad, pad + (28 if not narrow else 23)), sub, font=font(12), fill=0)
    top = pad + 50

    if variant == "A":
        lf = font(13, True) if not narrow else font(12, True)
        right_w = 0
        labels = []
        for s in MKT:
            lab = (f"{SHORT[s['label']]} {s['ratio']:.2f}×" if narrow
                   else f"{SHORT[s['label']]} {s['ratio']:.2f}×  {s['fmt'](s['last'])}")
            labels.append(lab)
            right_w = max(right_w, text_w(d, lab, lf))
        right_w += 10
        bottom = H - pad - 18
        legend_h = 0
    else:
        right_w = 6
        rows = len(MKT)
        row_h = 22 if not narrow else 20
        legend_h = rows * row_h + 36
        bottom = H - pad - 18 - legend_h

    left = pad + 30
    plot_r = w - pad - right_w
    lo = min(v for s in MKT for _, v in s["pts"])
    hi = max(v for s in MKT for _, v in s["pts"])
    llo, lhi = math.log(lo * 0.92), math.log(hi * 1.08)

    def Y(v):
        return bottom - (math.log(v) - llo) / (lhi - llo) * (bottom - top)

    def X(dt):
        return left + (dt - START).days / (TODAY - START).days * (plot_r - left)

    af = font(11)
    for g in (0.25, 0.5, 1, 1.5, 2, 3):
        if lo * 0.92 < g < hi * 1.08:
            y = Y(g)
            if g == 1:
                d.line([(left, y), (plot_r, y)], fill=0, width=2)
            else:
                for x in range(left, int(plot_r), 6):
                    d.point((x, y), fill=0)
            d.text((left - 4, y), f"{g:g}×", font=af, fill=0, anchor="rm")
    d.line([(left, bottom), (plot_r, bottom)], fill=0, width=1)
    for yr in range(START.year + 1, TODAY.year + 1):
        x = X(date(yr, 1, 1))
        d.line([(x, bottom), (x, bottom + 4)], fill=0)
        d.text((x, bottom + 6), f"'{yr % 100}" if narrow else str(yr), font=af, fill=0, anchor="mt")

    for k, s in enumerate(MKT):
        line_with_markers(d, [(X(dt), Y(v)) for dt, v in s["pts"]], s["style"], k, len(MKT))

    if variant == "A":
        ys = sorted(((Y(s["ratio"]), i) for i, s in enumerate(MKT)))
        min_gap = 17
        placed = []
        for y, i in ys:
            if placed and y - placed[-1][0] < min_gap:
                y = placed[-1][0] + min_gap
            placed.append((y, i))
        # shift up if overflowed
        over = placed[-1][0] - bottom
        if over > 0:
            placed = [(y - over, i) for y, i in placed]
        for y, i in placed:
            s = MKT[i]
            ly = Y(s["ratio"])
            d.line([(plot_r + 1, ly), (plot_r + 5, y)], fill=0)
            d.text((plot_r + 7, y), labels[i], font=lf, fill=0, anchor="lm")
    else:
        ty = bottom + 40
        tf, tb = font(12 if narrow else 13), font(12 if narrow else 13, True)
        cols = ([("", 34), ("", 44 if narrow else 90), ("now", 0), ("×avg", 0)] if narrow else
                [("", 34), ("", 90), ("now", 0), ("×avg", 0), ("1 yr", 0)])
        xs_right = ([w - pad - 60, w - pad] if narrow else [w - pad - 150, w - pad - 70, w - pad])
        hdr = [c for c, _ in cols[2:]]
        for x, h in zip(xs_right, hdr):
            d.text((x, ty - 4), h, font=font(11), fill=0, anchor="rb")
        d.line([(pad, ty), (w - pad, ty)], fill=0)
        for k, s in enumerate(MKT):
            y = ty + 12 + k * (20 if narrow else 22)
            sample(d, pad, y, s["style"], 24)
            d.text((pad + 32, y), SHORT[s["label"]] if narrow else s["label"], font=tb, fill=0, anchor="lm")
            vals = [s["fmt"](s["last"]), f"{s['ratio']:.2f}×"]
            if not narrow:
                vals.append(f"{s['yoy']*100:+.0f}%")
            for x, v in zip(xs_right, vals):
                d.text((x, y), v, font=tf, fill=0, anchor="rm")

    asof = min(s["asof"] for s in MKT)
    d.text((w - pad, pad + 4), f"as of {asof:%b %-d}", font=font(11), fill=0, anchor="ra")
    return img


# ---------------- Calendar + weather ----------------
def month_grid(d, x, y, w, today, big=False):
    cal = calendar.Calendar(firstweekday=6)
    weeks = cal.monthdayscalendar(today.year, today.month)
    cw = w / 7
    ch = 26 if big else 22
    hf = font(12 if big else 11, True)
    nf = font(15 if big else 13)
    for i, n in enumerate("SMTWTFS"):
        d.text((x + cw * i + cw / 2, y + ch / 2), n, font=hf, fill=0, anchor="mm")
    d.line([(x + 4, y + ch), (x + w - 4, y + ch)], fill=0)
    for r, wk in enumerate(weeks):
        for c, dn in enumerate(wk):
            if not dn:
                continue
            cx, cy = x + cw * c + cw / 2, y + ch * (r + 1) + ch / 2 + 2
            if dn == today.day:
                d.rounded_rectangle([cx - cw / 2 + 3, cy - ch / 2 + 1, cx + cw / 2 - 3, cy + ch / 2 - 1], 4, fill=0)
                d.text((cx, cy), str(dn), font=font(15 if big else 13, True), fill=255, anchor="mm")
            else:
                d.text((cx, cy), str(dn), font=nf, fill=0, anchor="mm")
    return ch * (len(weeks) + 1) + 4


def date_block(d, x, y, w, today, scale=1.0, center=True):
    anchor = "mt" if center else "lt"
    cx = x + w / 2 if center else x
    d.text((cx, y), today.strftime("%A").upper(), font=font(int(16 * scale), True), fill=0, anchor=anchor)
    d.text((cx, y + 18 * scale), str(today.day), font=font(int(72 * scale), True), fill=0, anchor=anchor)
    d.text((cx, y + 100 * scale), today.strftime("%B %Y"), font=font(int(16 * scale)), fill=0, anchor=anchor)
    return 124 * scale


def now_block(d, x, y, w, big=False):
    cur, daily = WX["current"], WX["daily"]
    cond, kind = wmo(cur["weather_code"])
    r = 34 if big else 28
    icon(d, kind, x + r + 4, y + r + 4, r)
    tx = x + 2 * r + 18
    d.text((tx, y + 2), f"{cur['temperature_2m']:.0f}°", font=font(46 if big else 40, True), fill=0)
    d.text((tx, y + (54 if big else 48)), cond, font=font(14), fill=0)
    d.text((tx, y + (72 if big else 65)),
           f"H {daily['temperature_2m_max'][0]:.0f}°  L {daily['temperature_2m_min'][0]:.0f}°",
           font=font(14, True), fill=0)
    return 2 * r + 20


def forecast_strip(d, x, y, w, days=5):
    daily = WX["daily"]
    cw = w / days
    for k in range(days):
        i = k + 1
        cx = x + cw * k + cw / 2
        dt = date.fromisoformat(daily["time"][i])
        d.text((cx, y), dt.strftime("%a"), font=font(13, True), fill=0, anchor="mt")
        icon(d, wmo(daily["weather_code"][i])[1], cx, y + 36, 15)
        d.text((cx, y + 58), f"{daily['temperature_2m_max'][i]:.0f}°", font=font(14, True), fill=0, anchor="mt")
        d.text((cx, y + 76), f"{daily['temperature_2m_min'][i]:.0f}°", font=font(13), fill=0, anchor="mt")
    return 96


def forecast_rows(d, x, y, w, days=5):
    daily = WX["daily"]
    rh = 30
    for k in range(days):
        i = k + 1
        yy = y + k * rh + rh / 2
        dt = date.fromisoformat(daily["time"][i])
        d.text((x + 4, yy), dt.strftime("%a"), font=font(15, True), fill=0, anchor="lm")
        icon(d, wmo(daily["weather_code"][i])[1], x + 70, yy, 12)
        d.text((x + w - 50, yy), f"{daily['temperature_2m_max'][i]:.0f}°", font=font(15, True), fill=0, anchor="rm")
        d.text((x + w - 4, yy), f"{daily['temperature_2m_min'][i]:.0f}°", font=font(15), fill=0, anchor="rm")
        if k:
            for xx in range(int(x + 4), int(x + w - 4), 4):
                d.point((xx, yy - rh / 2), fill=0)
    return days * rh


def calweather(variant, w):
    img = Image.new("L", (w, H), 255)
    d = ImageDraw.Draw(img)
    pad = 14
    iw = w - 2 * pad
    if w < 400:  # stacked column
        y = pad
        if variant == "A":
            y += date_block(d, pad, y, iw, TODAY, scale=0.8) + 2
            y += month_grid(d, pad, y, iw, TODAY) + 8
            d.line([(pad, y), (w - pad, y)], fill=0); y += 10
            y += now_block(d, pad, y, iw) + 8
            d.line([(pad, y), (w - pad, y)], fill=0); y += 8
            forecast_strip(d, pad, y, iw)
        else:
            y += now_block(d, pad, y, iw, big=True) + 4
            y += forecast_rows(d, pad, y, iw) + 8
            d.line([(pad, y), (w - pad, y)], fill=0); y += 8
            d.text((pad, y), TODAY.strftime("%a, %B %-d"), font=font(20, True), fill=0); y += 30
            month_grid(d, pad, y, iw, TODAY)
    else:  # two columns
        split = int(w * (0.45 if w < 700 else 0.36))
        lx, lw, rx, rw = pad, split - 2 * pad, split + pad, w - split - 2 * pad
        d.line([(split, pad), (split, H - pad)], fill=0)
        if variant == "A":
            y = pad + 6
            y += date_block(d, lx, y, lw, TODAY, scale=1.15) + 16
            month_grid(d, lx, y, lw, TODAY, big=True)
            y = pad + 10
            y += now_block(d, rx, y, rw, big=True) + 30
            d.line([(rx, y), (rx + rw, y)], fill=0); y += 14
            d.text((rx, y), "Next days", font=font(13, True), fill=0); y += 24
            forecast_strip(d, rx, y, rw, days=5)
        else:
            y = pad + 6
            y += now_block(d, lx, y, lw, big=True) + 14
            forecast_rows(d, lx, y, lw)
            y = pad + 6
            d.text((rx, y), TODAY.strftime("%A"), font=font(18, True), fill=0)
            d.text((rx, y + 24), TODAY.strftime("%B %-d, %Y" if w >= 700 else "%b %-d, %Y"), font=font(26, True), fill=0)
            month_grid(d, rx, y + 70, rw, TODAY, big=True)
    return img


# ---------------- output ----------------
def to1bit(img):
    return img.point(lambda p: 255 if p > 140 else 0).convert("1")


def footer(img):
    d = ImageDraw.Draw(img)
    d.line([(0, 464), (800, 464)], fill=0)
    d.text((8, 472), "updated 6:00 PM", font=font(10), fill=0, anchor="lm")
    d.text((792, 472), "inkboard", font=font(10), fill=0, anchor="rm")


OUT.mkdir(parents=True, exist_ok=True)
for v in "AB":
    for name, w in SIZES.items():
        to1bit(market(v, w)).save(OUT / f"mkt-{v}-{name}.png")
        to1bit(calweather(v, w)).save(OUT / f"cal-{v}-{name}.png")
    screen = Image.new("L", (800, 480), 255)
    screen.paste(market(v, 534), (0, 0))
    screen.paste(calweather(v, 266), (534, 0))
    ImageDraw.Draw(screen).line([(534, 8), (534, 456)], fill=0)
    footer(screen)
    to1bit(screen).save(OUT / f"screen-{v}.png")
print("ok")
