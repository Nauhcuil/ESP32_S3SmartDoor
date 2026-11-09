import sys, time, serial
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + secs
with open(out, "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d)
            f.flush()
s.close()
