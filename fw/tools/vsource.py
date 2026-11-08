#!/usr/bin/env python3
"""vsource.py - the switching between the head unit's music and the phones, for the firmware in QEMU
(docs/firmware.md, "Перемикання джерел").

In real time, over the emulated Ethernet, this script is at once:
  the ring     sink 1 as the head unit plays it: an idle AUX input (noise at -80 dBFS), a radio and a CD (440 Hz,
               the CD with a 3.5 s pause between tracks), and the head unit's telegrams at two source changes;
  a phone      the virtual phone (660 Hz, 44.1 kHz, +300 ppm, in A2DP-like bursts): asked to pause, it stops its
               stream, asked to play, it starts it again, as a phone does;
  an iPhone    an AirPlay sender (880 Hz) naming its remote control (DACP) on port 3689: pause stops its packets with
               a FLUSH, play sends them again.
It takes sink 1 back, finds which of the three is heard every 0.1 s, checks that against the expected timeline and the
pause and play requests against when they are due, looks for clicks and checks that the head unit's music passes bit
for bit, then writes the result and plays it on the PC's sound card.

Run it with the ESP-IDF Python (it has the cryptography package), then QEMU with the ports forwarded as for vraop.py:
    python vsource.py [--no-play]
    idf.py -B build_qemu qemu --qemu-extra-args "-nic user,model=open_eth,hostfwd=tcp::5000-:5000,hostfwd=udp::6000-:6000,hostfwd=udp::6001-:6001"
"""
import argparse
import array
import base64
import http.server
import math
import os
import random
import socket
import struct
import sys
import threading
import time

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vamp  # noqa: E402  the head unit's telegrams
import vphone as vp  # noqa: E402  the ring's protocol, writing the result
import vraop as vr  # noqa: E402  the AirPlay sender's parts

RATE, BLOCK = vp.RING, vp.PULL                  # 48 kHz, 10 ms per ring block
END = 56.0
AMP = 0.25                                      # every source's tone, -12 dBFS
F_HU, F_PHONE, F_AIR = 440.0, 660.0, 880.0
PPM = 300                                       # both phones' clocks against the ring's
BURST = 512
DACP_PORT, DACP_ID, ACTIVE_REMOTE = 3689, '0123456789ABCDEF', '1234567890'
HU_MUSIC = [(5.0, 10.0), (20.0, 24.0), (27.5, 30.0), (42.0, 47.0)]     # radio, CD, CD after its pause, radio
TELEGRAMS = [
    (0.0, vamp.levels(1, 0)),                                           # sink 1 at 0 dB: the music passes as it is
    (30.0, vamp.tele(0x113, 2, [1, 1])), (30.3, vamp.tele(0x113, 2, [1, 0])),        # a source change: mute, unmute
    (47.0, vamp.tele(0x112, 2, [1])), (47.2, vamp.tele(0x111, 2, [1, 0, 0, 1, 2, 3])),  # another: disconnect, connect
]
AIRPLAY_SETUP, AIRPLAY_START = 34.0, 36.0
EXPECT = [                                      # what sink 1 plays, well inside each stretch
    ('silence', 0.3, 1.8),      # the head unit's music not known yet
    ('phone', 2.5, 5.0),        # silent for 2 s: the phone plays
    ('hu', 5.6, 9.9),           # the radio: it plays and the phone is paused
    ('silence', 10.2, 15.6),    # AUX with no source change seen: the phone waits for 5 s of silence
    ('phone', 16.6, 20.0),      # resumed
    ('hu', 20.6, 23.9),         # a CD
    ('silence', 24.3, 27.4),    # its pause between tracks: the phone stays paused
    ('hu', 27.8, 29.9),
    ('silence', 30.2, 32.8),    # AUX after a mute and unmute: 2 s of silence are enough
    ('phone', 33.6, 36.0),      # resumed
    ('airplay', 37.0, 42.0),    # the iPhone starts: the newer plays, the phone is paused
    ('hu', 42.6, 46.9),         # the radio: the iPhone is paused through its remote control
    ('silence', 47.2, 49.8),    # AUX after a disconnect and connect
    ('airplay', 51.0, 55.5),    # the iPhone resumed
]
EVENTS = [('phone', 'pause', 5.0, 5.8), ('phone', 'play', 15.5, 16.6), ('phone', 'pause', 20.0, 20.8),
          ('phone', 'play', 32.5, 33.6), ('phone', 'pause', 36.0, 37.2), ('airplay', 'pause', 42.0, 42.8),
          ('airplay', 'play', 49.5, 51.0)]


def hu_track():
    """Sink 1 as the head unit sends it, 16-bit mono: noise of an idle AUX input, music with 10 ms soft edges."""
    n, edge = int(END * RATE), int(0.010 * RATE)
    rnd = random.Random(1)
    x = array.array('h', (rnd.randint(-5, 5) for _ in range(n)))
    for t0, t1 in HU_MUSIC:
        a, b = int(t0 * RATE), int(t1 * RATE)
        for i in range(a, b):
            k = min(i - a, b - 1 - i)
            env = 1.0 if k >= edge else 0.5 - 0.5 * math.cos(math.pi * k / edge)
            x[i] += int(round(AMP * env * 32767 * math.sin(2 * math.pi * F_HU * i / RATE)))
    return x


def stereo(f, start, n):
    """n frames of an f Hz tone at 44.1 kHz from sample start, left = right."""
    m = array.array('h', (int(round(AMP * 32767 * math.sin(2 * math.pi * f * (start + i) / 44100))) for i in range(n)))
    st = array.array('h', bytes(4 * n))
    st[0::2] = m
    st[1::2] = m
    return st


def amp_at(x, f, fs):
    """The amplitude of the f Hz sine in x by least squares."""
    w = 2 * math.pi * f / fs
    cc = ss = cs = xc = xs = 0.0
    for i, v in enumerate(x):
        c, s = math.cos(w * i), math.sin(w * i)
        cc += c * c
        ss += s * s
        cs += c * s
        xc += v * c
        xs += v * s
    det = cc * ss - cs * cs
    return math.hypot((xc * ss - xs * cs) / det, (xs * cc - xc * cs) / det)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build_qemu'))
    ap.add_argument('--no-play', action='store_true')
    o = ap.parse_args()
    print("making the head unit's sink 1...", flush=True)
    hu = hu_track()
    pub = vr.airport_key().public_key()
    n_blocks = int(END * RATE / BLOCK)
    lock = threading.Lock()
    events, cmds = [], {}           # (time, who, what); the requests not yet acted on
    got = [0]                       # ring blocks back from the firmware: the output's time

    def now_out():
        return got[0] * BLOCK / RATE

    class Dacp(http.server.BaseHTTPRequestHandler):
        """The iPhone's remote control: GET /ctrl-int/1/<command> with its Active-Remote."""
        def do_GET(self):
            ok = self.headers.get('Active-Remote') == ACTIVE_REMOTE and self.path.startswith('/ctrl-int/1/')
            what = self.path.rsplit('/', 1)[-1] if ok else 'refused: ' + self.path
            self.send_response(204 if ok else 403)
            self.end_headers()
            with lock:
                events.append((now_out(), 'airplay', what))
                if ok:
                    cmds['airplay'] = what

        def log_message(self, *a):
            pass
    dacp = http.server.HTTPServer(('127.0.0.1', DACP_PORT), Dacp)
    threading.Thread(target=dacp.serve_forever, daemon=True).start()

    srv = socket.create_server(('127.0.0.1', vp.PORT))
    srv.settimeout(300)
    print(f'waiting for the firmware in QEMU (ring on port {vp.PORT})...', flush=True)
    ring, _ = srv.accept()
    ring.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    out, unders = bytearray(), []

    def reader():
        last = 0
        for b in range(n_blocks):
            typ, playing, under, n, fill, corr = vp.MSG_OUT.unpack(vp.recv_all(ring, vp.MSG_OUT.size))
            out.extend(vp.recv_all(ring, 8 * n))
            got[0] = b + 1
            if under != last and under:             # a phone stream's count goes up, or a new one starts at 0
                unders.append(round(now_out(), 2))
            last = under
            if typ in (ord('p'), ord('g')):
                with lock:
                    events.append((now_out(), 'phone', 'pause' if typ == ord('p') else 'play'))
                    cmds['phone'] = typ
    rd = threading.Thread(target=reader, daemon=True)

    # the iPhone: its session is set up in a thread of its own, so the ring keeps its pace meanwhile
    air = {'ready': threading.Event()}
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctrl.bind(('127.0.0.1', 0))
    timing = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    timing.bind(('127.0.0.1', 0))
    audio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def airplay_setup():
        rtsp = vr.Rtsp('127.0.0.1', vr.RTSP)
        hdr = [f'DACP-ID: {DACP_ID}', f'Active-Remote: {ACTIVE_REMOTE}']
        challenge = base64.b64encode(os.urandom(16)).decode().rstrip('=')
        rtsp.ask('OPTIONS', hdr + [f'Apple-Challenge: {challenge}'])
        key, iv = os.urandom(16), os.urandom(16)
        wrapped = pub.encrypt(key, padding.OAEP(mgf=padding.MGF1(hashes.SHA1()), algorithm=hashes.SHA1(), label=None))
        sdp = ('v=0\r\no=iTunes 1 0 IN IP4 10.0.2.2\r\ns=iTunes\r\nc=IN IP4 10.0.2.15\r\nt=0 0\r\n'
               'm=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\na=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n'
               f"a=rsaaeskey:{base64.b64encode(wrapped).decode().rstrip('=')}\r\n"
               f"a=aesiv:{base64.b64encode(iv).decode().rstrip('=')}\r\n")
        rtsp.ask('ANNOUNCE', hdr + ['Content-Type: application/sdp'], sdp.encode())
        rtsp.ask('SETUP', hdr + [f'Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;'
                                 f'control_port={ctrl.getsockname()[1]};timing_port={timing.getsockname()[1]}'])
        seq0 = random.randrange(65536)
        rtsp.ask('RECORD', hdr + ['Range: npt=0-', f'RTP-Info: seq={seq0};rtptime=0'])
        rtsp.ask('SET_PARAMETER', hdr + ['Content-Type: text/parameters'], b'volume: 0.000000\r\n')
        air.update(rtsp=rtsp, hdr=hdr, key=key, iv=iv, seq0=seq0)
        air['ready'].set()

    sent, resent = {}, [0]

    def airplay_packet(k):
        payload = vr.alac_escape(stereo(F_AIR, k * vr.FRAMES, vr.FRAMES))
        whole = len(payload) & ~15
        enc = Cipher(algorithms.AES(air['key']), modes.CBC(air['iv'])).encryptor()
        payload = enc.update(payload[:whole]) + enc.finalize() + payload[whole:]
        seq = (air['seq0'] + k) & 0xFFFF
        pkt = struct.pack('>BBHII', 0x80, 0xE0 if k == 0 else 0x60, seq, k * vr.FRAMES, 0x4A4C5221) + payload
        with lock:
            sent[seq] = pkt
            sent.pop((seq - 400) & 0xFFFF, None)
        return pkt

    def control():
        """What the receiver asks to be sent again, as an iPhone answers it."""
        while True:
            b, _ = ctrl.recvfrom(64)
            if len(b) >= 8 and b[1] == 0xD5:
                first, count = struct.unpack('>HH', b[4:8])
                for i in range(count):
                    with lock:
                        p = sent.get((first + i) & 0xFFFF)
                    if p:
                        audio.sendto(b'\x80\xd6\x00\x01' + p, ('127.0.0.1', vr.CTRL))
                        resent[0] += 1
    threading.Thread(target=control, daemon=True).start()

    def flush(k):
        air['rtsp'].ask('FLUSH', air['hdr'] + [f"RTP-Info: seq={(air['seq0'] + k) & 0xFFFF};rtptime={k * vr.FRAMES}"])

    fp = 44100 * (1 + PPM * 1e-6)
    pending = list(TELEGRAMS)
    v_on, v_k, v_t0, v_pos = True, 0, 0.0, 0        # the phone: streaming, bursts since its start, then, position
    a_on, a_k, a_k0, a_t0, a_started = False, 0, 0, 0.0, False
    setup = None
    j = 0
    t0 = time.perf_counter()
    rd.start()
    ring.sendall(vp.MSG_IN.pack(ord('R'), 44100))
    print(f'running {END:.0f} s in real time...', flush=True)
    while j < n_blocks:
        now = time.perf_counter() - t0
        with lock:
            pc, ac = cmds.pop('phone', None), cmds.pop('airplay', None)
        if pc == ord('p') and v_on:                     # the phone pauses: its stream stops
            v_on = False
            ring.sendall(vp.MSG_IN.pack(ord('X'), 0))
        elif pc == ord('g') and not v_on:               # and plays again: a new stream
            v_on, v_k, v_t0 = True, 0, now
            ring.sendall(vp.MSG_IN.pack(ord('R'), 44100))
        if ac == 'pause' and a_on:                      # the iPhone pauses: no more packets, then a FLUSH
            a_on = False
            threading.Thread(target=flush, args=(a_k,), daemon=True).start()
        elif ac == 'play' and a_started and not a_on:
            a_on, a_k0, a_t0 = True, a_k, now
        if setup is None and now >= AIRPLAY_SETUP:
            setup = threading.Thread(target=airplay_setup, daemon=True)
            setup.start()
        if not a_started and now >= AIRPLAY_START and air['ready'].is_set():
            a_on, a_started, a_k0, a_t0 = True, True, 0, now
        t_ring = j * BLOCK / RATE
        t_v = v_t0 + v_k * BURST / fp if v_on else math.inf
        t_a = a_t0 + (a_k - a_k0) * vr.FRAMES / fp if a_on else math.inf
        if t_v <= now:
            ring.sendall(vp.MSG_IN.pack(ord('I'), BURST) + stereo(F_PHONE, v_pos, BURST).tobytes())
            v_k += 1
            v_pos += BURST
        elif t_a <= now:
            audio.sendto(airplay_packet(a_k), ('127.0.0.1', vr.AUDIO))
            a_k += 1
        elif t_ring <= now:
            while pending and pending[0][0] <= t_ring:
                t = pending.pop(0)[1]
                ring.sendall(vp.MSG_IN.pack(ord('C'), len(t)) + t)
            inter = array.array('h', bytes(10 * BLOCK))
            inter[0::5] = hu[j * BLOCK:(j + 1) * BLOCK]
            inter[1::5] = hu[j * BLOCK:(j + 1) * BLOCK]
            ring.sendall(vp.MSG_IN.pack(ord('S'), BLOCK) + inter.tobytes())
            j += 1
        else:
            time.sleep(max(0.0, min(0.002, min(t_ring, t_v, t_a) - now)))
    rd.join(timeout=30)
    if air['ready'].is_set():
        air['rtsp'].ask('TEARDOWN', air['hdr'])
    ring.sendall(vp.MSG_IN.pack(ord('Q'), 0))
    ring.close()
    dacp.shutdown()

    frames = array.array('i', bytes(out))
    os.makedirs(o.out, exist_ok=True)
    wav_path = os.path.join(o.out, 'vsource_out.wav')
    vp.write_wav(wav_path, frames)
    print('\nresult')
    left = frames[0::2]
    names = (('hu', F_HU), ('phone', F_PHONE), ('airplay', F_AIR))
    w, dec = RATE // 10, 4
    heard = []
    for k in range(len(left) // w):
        x = [left[k * w + i] / 2147483648.0 for i in range(0, w, dec)]
        a = {name: amp_at(x, f, RATE / dec) for name, f in names}
        best = max(a, key=a.get)
        heard.append(best if a[best] > 0.01 else 'silence')
    segs, start = [], 0
    for k in range(1, len(heard) + 1):
        if k == len(heard) or heard[k] != heard[start]:
            segs.append((start / 10, k / 10, heard[start]))
            start = k
    print('  sink 1 plays: ' + ', '.join(f'{a:.1f}-{b:.1f} {n}' for a, b, n in segs))
    bad = [(n, a, b) for n, a, b in EXPECT if any(heard[k] != n for k in range(int(a * 10), min(len(heard), int(b * 10))))]
    print(f"  against the timeline: {'all ' + str(len(EXPECT)) + ' stretches as expected' if not bad else 'WRONG ' + str(bad)}")
    print('  requests: ' + ', '.join(f'{t:.2f} {who} {what}' for t, who, what in events))
    print(f"  AirPlay packets sent again on request: {resent[0]}; phone stream underruns at: {unders or 'none'}")
    left_ev = list(events)
    missing = []
    for who, what, a, b in EVENTS:
        hit = next((e for e in left_ev if e[1] == who and e[2] == what and a <= e[0] <= b), None)
        if hit:
            left_ev.remove(hit)
        else:
            missing.append((who, what, a, b))
    print(f"  requests against the expected: {'all ' + str(len(EVENTS)) + ' in time' if not missing and not left_ev else ''}"
          f"{' MISSING ' + str(missing) if missing else ''}{' UNEXPECTED ' + str(left_ev) if left_ev else ''}")
    step = max(abs(left[i + 1] - left[i]) for i in range(len(left) - 1)) / 2147483648.0
    limit = AMP * 2 * math.pi * F_AIR / RATE * 1.2
    print(f'  largest step between samples {step:.4f} of full scale (the 880 Hz tone alone makes {limit / 1.2:.4f}): '
          f"{'no clicks' if step <= limit else 'A CLICK'}")
    diff = total = 0
    for n, a, b in EXPECT:
        if n == 'hu':
            for i in range(int(a * RATE), int(b * RATE)):
                total += 2
                diff += (frames[2 * i] != hu[i] << 16) + (frames[2 * i + 1] != hu[i] << 16)
    print(f"  the head unit's music: {total} samples, {diff} differ from what the ring brought")
    print(f'  written: {os.path.abspath(wav_path)}', flush=True)
    if not o.no_play and sys.platform == 'win32':
        import winsound
        print("  playing it on the PC's sound card...", flush=True)
        winsound.PlaySound(wav_path, winsound.SND_FILENAME)
    return 0 if not bad and not missing and not left_ev and step <= limit and not diff else 1


if __name__ == '__main__':
    sys.exit(main())
