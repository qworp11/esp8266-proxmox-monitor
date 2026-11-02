# -*- coding: utf-8 -*-
# v3_usbcheck_life.py — USB-прогон 12 новых бытовых сцен волны v3.4
import sys, time, serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ser = serial.Serial()
ser.port = "COM3"
ser.baudrate = 115200
ser.timeout = 0.2
ser.open()
ser.dtr = False
ser.rts = False
time.sleep(0.2)
ser.rts = True   # EN -> LOW
time.sleep(0.15)
ser.rts = False  # EN -> HIGH, чистый старт


def pump(secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        data = ser.readline()
        if data:
            print(data.decode("utf-8", errors="replace").rstrip())
            sys.stdout.flush()


def cmd(tok, wait):
    print(f"\n>>> E|{tok}")
    ser.write(f"E|{tok}\n".encode())
    pump(wait)


try:
    print("=== BOOT, читаем 12с ===")
    pump(12)
    for tok in ("vacuum", "cook", "chat", "walk", "scared", "movie",
                "knit", "garden", "game", "music", "parcel", "busy"):
        cmd(tok, 7)
    cmd("hat", 2)
    cmd("none", 2)
    print("\n=== done ===")
finally:
    ser.close()
