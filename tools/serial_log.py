# Record what boards print at start-up, from one or more COM ports at once.
# Survives the USB port disappearing when the board resets (reopens it).
#   py -3.11 tools\serial_log.py COM26 COM25
import sys, time, threading
import serial

SECS = 40
out = open('p4_log.txt', 'w', encoding='utf-8', errors='replace')
lock = threading.Lock()

def say(port, text):
    with lock:
        for line in text.splitlines():
            s = f'{time.strftime("%H:%M:%S")} {port}: {line}'
            out.write(s + '\n'); out.flush()
            print(s.encode('ascii', 'replace').decode())

def watch(port):
    end = time.time() + SECS
    first = True
    while time.time() < end:
        try:
            p = serial.Serial(port, 115200, timeout=0.2)
            if first:  # reset the board through the port (same lines esptool uses), then just listen
                p.dtr = False; p.rts = True; time.sleep(0.1); p.rts = False
                first = False
            say(port, '--- port open ---')
            buf = b''
            while time.time() < end:
                d = p.read(4096)
                if not d: continue
                buf += d
                *lines, buf = buf.split(b'\n')
                for l in lines: say(port, l.decode('utf-8', 'replace').rstrip('\r'))
            p.close()
        except Exception as e:
            if first: say(port, f'(cannot open yet: {e.__class__.__name__})'); first = False
            time.sleep(0.3)

ports = sys.argv[1:] or ['COM26']
ts = [threading.Thread(target=watch, args=(p,), daemon=True) for p in ports]
for t in ts: t.start()
for t in ts: t.join()
out.close()
print('Saved p4_log.txt')
