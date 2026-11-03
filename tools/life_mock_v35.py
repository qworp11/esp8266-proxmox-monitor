# -*- coding: utf-8 -*-
# life_mock_v35.py — превью фич v3.5: pet (дужки + сердце-мурр) и день рождения (торт со свечкой)
# Рендерится тем же моком-стилем: кейфреймы лист + GIF
import math
import os, sys
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from scenes_mock import sheet, gif, spark
from life_mock_v34 import draw_eye, EYE1_X, EYE2_X

FPS = 12
EYE_OK_H = 38


def draw_happy(d, dy=0):
    for cx in (EYE1_X + 15, EYE2_X + 15):
        d.arc((cx - 10, 32 + dy - 8, cx + 10, 32 + dy + 8), 200, 340, fill="black", width=3)


def draw_heart(d, hy, hs):
    rv = int(3.4 * hs + 0.5)
    rw = int(3.6 * hs + 0.5)
    tip = int(8.0 * hs + 0.5)
    cy = int(hy + 0.5)
    for dx in (-rw, rw):
        d.ellipse((64 + dx - rv, cy - rv, 64 + dx + rv, cy + rv), fill="black")
    d.polygon((63 - rw, cy - 1, 65 + rw, cy - 1, 64, cy + tip), fill="black")


def draw_cake(d, base, flick, blow):
    d.rectangle((48, base + 10, 79, base + 13), fill="black")          # тарелка
    if base < 61:
        d.rectangle((50, base, 77, base + 9), fill="black")            # корж
        d.rectangle((52, base + 3, 75, base + 5), fill="white")        # крем-негатив
        d.rectangle((63, base - 5, 64, base - 1), fill="black")        # свечка
        if blow:
            if flick & 1:
                d.point((57, base - 5), fill="black")
                d.point((70, base - 7), fill="black")
                d.point((66, base - 9), fill="black")
        else:
            fx = 63 if (flick & 1) else 64
            d.rectangle((fx, base - 9, fx + 1, base - 6), fill="black")


def eyes_up(d, py=4.0, r=7.0):
    draw_eye(d, EYE1_X, 12, EYE_OK_H, 0, py, r, 0, 0, 0)
    draw_eye(d, EYE2_X, 12, EYE_OK_H, 0, py, r, 0, 0, 0)


def frame_pet(t):
    img = Image.new("RGB", (128, 64), "white")
    d = ImageDraw.Draw(img)
    dy = int(0.8 * math.sin(t * 3.1))
    draw_happy(d, dy)
    hs = 1.0 + 0.22 * math.sin(t * 22.0)          # пульс ~3.5 Гц как в прошивке
    draw_heart(d, 13.5, hs)
    return img


def cake_base(t):
    # из прошивки drawCakeV3: подъём 1.6с, стоит 4с, уезжает 2.4с, всего 8.2с
    if t < 1.6:
        return 64 - int(18 * t / 1.6)
    if t < 5.6:
        return 46
    if t < 8.0:
        return 46 + int(18 * (t - 5.6) / 2.4)
    return 64


def frame_cake(t):
    img = Image.new("RGB", (128, 64), "white")
    d = ImageDraw.Draw(img)
    eyes_up(d, 6.0, 7.0)                          # смотрит вниз на торт
    base = cake_base(t)
    if base <= 60:
        blow = t >= 5.6
        draw_cake(d, base, int(t * 1000 / 130) if not blow else int(t * 1000 / 170), blow)
    return img


if __name__ == "__main__":
    DEL = 1000 // FPS
    picks = []
    allf = []
    # PET: мурчание 2.4с
    for i in range(int(2.4 * FPS)):
        t = i / FPS
        allf.append((frame_pet(t), DEL))
    for tt, name in ((0.5, "PET  мурчание x1.4"), (1.7, "PET  мурчание x0.9")):
        picks.append((name, [(frame_pet(tt), DEL)]))
    # CAKE: 8с
    for i in range(int(8.0 * FPS)):
        t = i / FPS
        allf.append((frame_cake(t), DEL))
    for tt, name in ((1.2, "CAKE  торт поднимается"), (3.5, "CAKE  свечка горит"),
                     (6.2, "CAKE  задули + искры"), (7.6, "CAKE  уезжает")):
        picks.append((name, [(frame_cake(tt), DEL)]))
    sheet(picks, r"E:\nodemcuv3\life_v35_preview.png")
    gif(allf, r"E:\nodemcuv3\life_v35_preview.gif", sc=3)
    print("LIFE v35 MOCK DONE")
