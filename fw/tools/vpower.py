#!/usr/bin/env python3
"""vpower.py - sleep for the firmware in QEMU (docs/firmware.md, "Сон").

QEMU has no ring and no supply to switch, so this script plays the ring's light and the master, and listens to the
sound engine's output in real time while the head unit's music (440 Hz) plays:
  1. the light goes for 2 s and comes back: no sleep, the music goes on;
  2. the master's NetBlock ShutDown Query, then Execute: the music fades within half a second (the firmware logs REM
     off, the DAC muted, HOLD released); it starts afresh 20 s later, the light having stayed;
  3. after the restart the light goes for good: the music fades about 5 s later.

    python vpower.py            (start it first, then QEMU as for vphone.py)
"""
import array
import math
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vamp  # noqa: E402  the head unit's telegrams
import vphone as vp  # noqa: E402  the link's framing

RATE, BLOCK = vp.RING, vp.PULL
GW = 0x161


def shutdown(control):
    """NetBlock ShutDown Start [control] from the master: 00h Query, 02h Execute."""
    return struct.pack('>HBBHBB', GW, 0x01, 0x00, 0x006, 0x0, 1) + bytes([control])


class Run:
    """One connection: ring blocks of music out in real time, the output's level per 0.1 s back."""

    def __init__(self, c):
        self.c, self.levels, self.t0, self.lock = c, [], None, threading.Lock()
        self.stop = threading.Event()
        self.block = array.array('h', bytes(10 * BLOCK))
        tone = [int(round(0.25 * 32767 * math.sin(2 * math.pi * 440 * i / RATE))) for i in range(BLOCK)]
        self.block[0::5] = array.array('h', tone)       # 480 frames = 22 periods of 440 Hz: blocks join seamlessly
        self.block[1::5] = array.array('h', tone)
        t = vamp.levels(1, 0)                          # sink 1 at 0 dB: the music as it comes
        self.c.sendall(vp.MSG_IN.pack(ord('C'), len(t)) + t)
        threading.Thread(target=self.writer, daemon=True).start()
        threading.Thread(target=self.reader, daemon=True).start()

    def send(self, typ, n, payload=b''):
        with self.lock:
            self.c.sendall(vp.MSG_IN.pack(ord(typ), n) + payload)

    def writer(self):
        self.t0 = time.perf_counter()
        k = 0
        raw = self.block.tobytes()
        while not self.stop.is_set():
            wait = self.t0 + k * BLOCK / RATE - time.perf_counter()
            if wait > 0:
                time.sleep(wait)
            try:
                self.send('S', BLOCK, raw)
            except OSError:
                return
            k += 1

    def reader(self):
        acc, n = 0.0, 0
        try:
            while True:
                typ, _, _, k, _, _ = vp.MSG_OUT.unpack(vp.recv_all(self.c, vp.MSG_OUT.size))
                fr = array.array('i', vp.recv_all(self.c, 8 * k))
                for v in fr[0::2]:
                    acc += (v / 2147483648.0) ** 2
                n += k
                if n >= RATE // 10:
                    self.levels.append((time.perf_counter() - self.t0, 10 * math.log10(acc / n + 1e-20)))
                    acc, n = 0.0, 0
        except (OSError, ConnectionError, struct.error):
            pass

    def now(self):
        return time.perf_counter() - self.t0

    def level(self, a, b):
        """The loudest 0.1 s between a and b seconds of this run."""
        v = [db for t, db in self.levels if a <= t <= b]
        return max(v) if v else None

    def first_quiet(self, after):
        return next((t for t, db in self.levels if t > after and db < -90), None)


def main():
    srv = socket.create_server(('127.0.0.1', vp.PORT))
    srv.settimeout(300)
    fails = []
    print(f'waiting for the firmware in QEMU on port {vp.PORT}...', flush=True)
    c, _ = srv.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    r = Run(c)
    r.send('K', 1)
    time.sleep(2.0)
    t_dark = r.now()
    r.send('K', 0)                                   # 1. the light flickers: 2 s dark
    time.sleep(2.0)
    r.send('K', 1)
    time.sleep(1.5)
    lv = r.level(t_dark + 2.5, r.now())
    ok = lv is not None and lv > -30
    print(f'1. light gone for 2 s and back: music after it {lv:.1f} dBFS ({"no sleep" if ok else "WRONG"})')
    if not ok:
        fails.append('flicker')
    r.send('C', 9, shutdown(0x00))                   # 2. ShutDown: Query, then Execute
    time.sleep(1.0)
    lv_q = r.level(r.now() - 0.8, r.now())
    t_exec = r.now()
    r.send('C', 9, shutdown(0x02))
    time.sleep(1.5)
    quiet = r.first_quiet(t_exec)
    ok = lv_q is not None and lv_q > -30 and quiet is not None and quiet - t_exec < 0.8
    print(f'2. ShutDown Query: music still {lv_q:.1f} dBFS; Execute: silent '
          f'{f"{quiet - t_exec:.2f} s after" if quiet else "NEVER"} ({"as due" if ok else "WRONG"})')
    if not ok:
        fails.append('ShutDown')
    print('   waiting for the restart (the light stayed)...', flush=True)
    r.stop.set()
    c2, _ = srv.accept()                             # 3. afresh: the light goes for good
    c2.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    c.close()
    r2 = Run(c2)
    r2.send('K', 1)
    time.sleep(2.0)
    t_dark = r2.now()
    r2.send('K', 0)
    time.sleep(7.0)
    quiet = r2.first_quiet(t_dark)
    lv = r2.level(t_dark + 0.5, t_dark + 4.8)
    ok = quiet is not None and 5.0 <= quiet - t_dark <= 6.0 and lv is not None and lv > -30
    print(f'3. light gone for good: music {lv:.1f} dBFS meanwhile, silent '
          f'{f"{quiet - t_dark:.2f} s after" if quiet else "NEVER"} ({"as due" if ok else "WRONG"})')
    if not ok:
        fails.append('light gone')
    r2.stop.set()
    try:
        r2.send('Q', 0)
    except OSError:
        pass
    c2.close()
    print(f'\nresult: {"all passed" if not fails else "FAILED: " + ", ".join(fails)}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
