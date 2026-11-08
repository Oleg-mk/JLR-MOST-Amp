#!/usr/bin/env python3
"""vamp.py - the head unit and the ring for the sound engine in QEMU (docs/firmware.md, "Обробка").

The sound engine (main/dsp.c) takes the head unit's commands through the amplifier emulation and the sinks' sound
from the ring. QEMU has no ring, so this script sends both over the emulated Ethernet (vphone.c's 'C' and 'S'
messages) in the ring's time and takes the output back:
  sink 1  the music: a 1 kHz tone, chords, a three-tone test signal (50 Hz, 1 kHz, 12 kHz), chords
  sink 2  a navigation prompt; sink 3 parking-sensor beeps; sink 4 a chime
and the head unit's timeline: a level for every sink, MuteParameters, Mute of sink 1 under the prompt, Bass and
Treble, then everything neutral. It checks the level, the mute's fade and silence, the mixing, the tone at three
frequencies and that the neutral end passes bit for bit, then writes the result and plays it on the PC's sound card.

    python vamp.py [--no-play]       (start it first, then QEMU as for vphone.py)
"""
import argparse
import array
import math
import os
import socket
import struct
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vphone as vp  # noqa: E402  the ring's protocol, writing the result

RATE, BLOCK, SECONDS = 48000, 480, 20
N = RATE * SECONDS
HU = 0x0161                         # the gateway, where the head unit's commands come from


def tele(fkt, op, data):
    """A head unit's telegram to the amplifier 22.02 (QEMU starts as Hi-Line), as vphone.c's 'C' takes it."""
    return bytes([HU >> 8, HU & 0xFF, 0x22, 0x02, fkt >> 8, fkt & 0xFF, op, len(data)]) + bytes(d & 0xFF for d in data)


def levels(sink, db):
    return tele(0x46D, 0, [sink, 0] + [db] * 5)        # SpeakerLevel: the row, 0 = all speakers, fl c fr rl rr (usb2most)


TIMELINE = [
    (0.0, levels(1, -10)), (0.0, levels(2, -6)), (0.0, levels(3, -12)), (0.0, levels(4, -8)),
    (0.0, tele(0x466, 0, [1, 0x01, 0x2C, 0x01, 0xF4, 2, 3])),     # sink 1: mute 300 ms degressive, back 500 ms progressive
    (5.5, tele(0x113, 2, [1, 1])), (8.0, tele(0x113, 2, [1, 0])),  # sink 1 under the navigation prompt
    (11.5, tele(0x202, 0, [4])), (13.0, tele(0x203, 0, [-4])),     # Bass +4, Treble -4 steps
    (15.0, tele(0x202, 0, [0])), (15.0, tele(0x203, 0, [0])),
    (17.0, levels(1, 0)),                                         # neutral from here: bit for bit
]


def sinks():
    """The four sinks as 16-bit arrays: sink 1 left, right, sinks 2, 3, 4."""
    l, two, three, four = (array.array('h', bytes(2 * N)) for _ in range(4))
    chords = ((261.63, 329.63, 392.00), (196.00, 246.94, 293.66), (220.00, 261.63, 329.63), (174.61, 220.00, 261.63))

    def music(i):
        t = i / RATE
        if t < 3:
            return 0.5 * math.sin(2 * math.pi * 1000 * t)
        if 11 <= t < 15:
            return 0.125 * sum(math.sin(2 * math.pi * f * t) for f in (50, 1000, 12000))
        # held chords, 2 s each with 50 ms edges: the music stays on, so its ducking under the prompt is heard
        u = (t - 3) % 8
        notes, tt = chords[int(u // 2)], u % 2
        env = min(1.0, tt / 0.05, (2 - tt) / 0.05)
        return 0.1 * env * sum(math.sin(2 * math.pi * f * tt) + 0.5 * math.sin(4 * math.pi * f * tt) +
                               0.25 * math.sin(6 * math.pi * f * tt) for f in notes)

    def prompt(t):
        """A navigation prompt: a two-note chime, then a voice-like buzz - harmonics of a gliding 140-180 Hz pitch
        shaped by the formants of an open vowel, in syllables four a second."""
        if t < 0.5:
            f, s = (784, t) if t < 0.25 else (659, t - 0.25)
            return 0.5 * math.sin(2 * math.pi * f * s) * math.exp(-s / 0.15)
        s = t - 0.5
        ph = 2 * math.pi * (140 * s + 20 * s * s)          # pitch 140 Hz rising to 180 Hz over 1 s
        f0 = 140 + 40 * s
        v = sum(math.sin(k * ph) * (math.exp(-((k * f0 - 700) / 160) ** 2) + 0.6 * math.exp(-((k * f0 - 1150) / 200) ** 2)
                                    + 0.25 * math.exp(-((k * f0 - 2600) / 300) ** 2) + 0.03)
                for k in range(1, 22))
        return 0.22 * v * abs(math.sin(math.pi * 4 * s)) ** 0.6
    for i in range(N):
        t = i / RATE
        l[i] = int(round(music(i) * 32767))
        if 5.5 <= t < 7.5:
            two[i] = int(round(max(-1.0, min(1.0, prompt(t - 5.5))) * 32767))
        if 9 <= t < 11 and (t - 9) % 0.2 < 0.08:
            three[i] = int(round(0.25 * math.sin(2 * math.pi * 1200 * t) * 32767))
        if 3.5 <= t < 5.5:
            four[i] = int(round(0.3 * (math.sin(2 * math.pi * 880 * t) + 0.6 * math.sin(2 * math.pi * 1320 * t)) *
                                math.exp(-(t - 3.5) / 0.6) * 32767))
    r = array.array('h', l)
    return l, r, two, three, four


def fit(x, f):
    """The amplitude of the f Hz sine in x by least squares."""
    w = 2 * math.pi * f / RATE
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
    print('making the sinks...', flush=True)
    l, r, two, three, four = sinks()
    inter = array.array('h', bytes(2 * 5 * N))
    for k, a in enumerate((l, r, two, three, four)):
        inter[k::5] = a
    raw = inter.tobytes()

    srv = socket.create_server(('127.0.0.1', vp.PORT))
    srv.settimeout(300)
    print(f'waiting for the firmware in QEMU on port {vp.PORT}...', flush=True)
    c, _ = srv.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    blocks = N // BLOCK

    def writer():
        pending = list(TIMELINE)
        for b in range(blocks):
            while pending and pending[0][0] * RATE <= b * BLOCK:
                t = pending.pop(0)[1]
                c.sendall(vp.MSG_IN.pack(ord('C'), len(t)) + t)
            c.sendall(vp.MSG_IN.pack(ord('S'), BLOCK) + raw[10 * BLOCK * b:10 * BLOCK * (b + 1)])
        c.sendall(vp.MSG_IN.pack(ord('Q'), 0))
    threading.Thread(target=writer, daemon=True).start()
    out = bytearray()
    for b in range(blocks):
        _, _, _, n, _, _ = vp.MSG_OUT.unpack(vp.recv_all(c, vp.MSG_OUT.size))
        out += vp.recv_all(c, 8 * n)
    c.close()
    y = array.array('i', bytes(out))
    yl = y[0::2]

    def sec(a, b):
        return range(int(a * RATE), int(b * RATE))
    print('\nresult')
    # the level: sink 1 at -10 dB, nothing else playing
    num = sum(float(yl[i]) ** 2 for i in sec(1, 2.9))
    den = sum(float(l[i] << 16) ** 2 for i in sec(1, 2.9))
    print(f'  level of sink 1, set to -10 dB: {10 * math.log10(num / den):.4f} dB')
    # under the prompt: sink 1 faded out within 300 ms, then only sink 2 at -6 dB
    g2 = 10 ** (-6 / 20)
    rest = [abs(yl[i] - (two[i] << 16) * g2) for i in sec(5.5, 7.4)]
    steps = [max(rest[int(k * 0.1 * RATE):int((k + 1) * 0.1 * RATE)]) / 2 ** 31 for k in range(4)]
    print('  sink 1 while muting (degressive, 300 ms), peak per 0.1 s: ' + ', '.join(f'{v:.2e}' for v in steps) +
          f'; after: {max(rest[int(0.4 * RATE):]) / 2 ** 31:.1e} of full scale')
    mix = max(abs(y[2 * i] - y[2 * i + 1]) for i in sec(5.9, 7.4))
    print(f'  the prompt (sink 2) alone at -6 dB, left minus right at most {mix} (the same in both)')
    back = sum(float(yl[i]) ** 2 for i in sec(8.6, 9.0)) / sum(float(l[i] << 16) ** 2 for i in sec(8.6, 9.0))
    print(f'  sink 1 back after unmute (500 ms progressive): {10 * math.log10(back):.3f} dB (the level -10 is back)')
    # the tone: 50 Hz, 1 kHz, 12 kHz against the time before Bass
    ref = {f: fit([yl[i] for i in sec(11.05, 11.45)], f) for f in (50, 1000, 12000)}
    for name, a, b, want in (('Bass +4 (+8 dB)', 12.0, 12.95, '+7.47 / 0 / 0'),
                             ('Bass +4, Treble -4', 13.5, 14.95, '+7.47 / 0 / -5.84')):
        got = [20 * math.log10(fit([yl[i] for i in sec(a, b)], f) / ref[f]) for f in (50, 1000, 12000)]
        print(f'  {name}: 50 Hz {got[0]:+.2f}, 1 kHz {got[1]:+.2f}, 12 kHz {got[2]:+.2f} dB (designed {want})')
    # neutral: every sample as it came in
    bad = sum((y[2 * i] != l[i] << 16) + (y[2 * i + 1] != r[i] << 16) for i in sec(17.1, SECONDS))
    print(f'  neutral from 17 s: {len(sec(17.1, SECONDS))} frames, {bad} samples differ from the input')
    os.makedirs(o.out, exist_ok=True)
    path = os.path.join(o.out, 'vamp_out.wav')
    vp.write_wav(path, y)
    print(f'  written: {os.path.abspath(path)}', flush=True)
    if not o.no_play and sys.platform == 'win32':
        import winsound
        print("  playing it on the PC's sound card...", flush=True)
        winsound.PlaySound(path, winsound.SND_FILENAME)
    return 0


if __name__ == '__main__':
    sys.exit(main())
