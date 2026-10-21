import sys, time, serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# Читает Serial COM3 (115200) заданное время.
# Перед чтением делает сброс платы через RTS (как esptool), чтобы поймать [BOOT].
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
ser = serial.Serial()
ser.port = "COM3"
ser.baudrate = 115200
ser.timeout = 0.2
ser.open()
ser.dtr = False
ser.rts = False
time.sleep(0.2)
ser.rts = True   # EN -> LOW, сброс
time.sleep(0.15)
ser.rts = False  # EN -> HIGH, чистый старт приложения
t0 = time.time()
try:
    while time.time() - t0 < secs:
        data = ser.readline()
        if data:
            print(data.decode("utf-8", errors="replace").rstrip())
            sys.stdout.flush()
finally:
    ser.close()
