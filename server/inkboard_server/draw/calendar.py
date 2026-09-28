"""Month grid, Sunday first, today inverted."""
import calendar
from datetime import date

from PIL import ImageDraw

from .fonts import font


def month_weeks(year: int, month: int) -> list[list[int]]:
    return calendar.Calendar(firstweekday=6).monthdayscalendar(year, month)


def month_grid(d: ImageDraw.ImageDraw, x: float, y: float, w: float, today: date, big: bool = False) -> int:
    weeks = month_weeks(today.year, today.month)
    cw, ch = w / 7, (26 if big else 22)
    head, num, num_bold = font(12 if big else 11, bold=True), font(15 if big else 13), font(15 if big else 13, bold=True)
    for i, letter in enumerate("SMTWTFS"):
        d.text((x + cw * i + cw / 2, y + ch / 2), letter, font=head, fill=0, anchor="mm")
    d.line([(x + 4, y + ch), (x + w - 4, y + ch)], fill=0)
    for r, week in enumerate(weeks):
        for c, day in enumerate(week):
            if not day:
                continue
            cx, cy = x + cw * c + cw / 2, y + ch * (r + 1) + ch / 2 + 2
            if day == today.day:
                d.rounded_rectangle([cx - cw / 2 + 3, cy - ch / 2 + 1, cx + cw / 2 - 3, cy + ch / 2 - 1], 4, fill=0)
                d.text((cx, cy), str(day), font=num_bold, fill=255, anchor="mm")
            else:
                d.text((cx, cy), str(day), font=num, fill=0, anchor="mm")
    return int(ch * (len(weeks) + 1) + 4)
