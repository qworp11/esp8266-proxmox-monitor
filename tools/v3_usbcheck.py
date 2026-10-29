# -*- coding: utf-8 -*-
# v3_usbcheck.py — USB-верификация v3: чистый reset, лог старта, прогон сцен E|...
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
    cmd("book", 6)
    cmd("pc", 6)
    cmd("tiktok", 6)
    cmd("self", 6)
    cmd("coffee", 6)
    cmd("wake", 3)
    cmd("none", 3)
    print("\n=== done ===")
finally:
    ser.close()
