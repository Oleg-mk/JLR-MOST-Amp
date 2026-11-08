#!/usr/bin/env python3
"""vphone.py - the phone and the ring for the firmware in QEMU (docs/firmware.md, "Телефон").

QEMU has no Bluetooth radio and no I2S, so this script stands in for both ends of the phone's path. The QEMU variant
of the firmware (idf.py -B build_qemu -DQEMU=1 build, then idf.py -B build_qemu qemu) connects to it over the emulated
Ethernet. The script runs both clocks in simulated time: it sends the phone's 16-bit frames in A2DP-like bursts at the
phone's rate (an offset in ppm, jitter, a hold-up every 10 s) and asks for 48 kHz frames at the ring's rate. Then it
writes what came back, reports the rate control and plays the result on the PC's sound card.

    python vphone.py [--wav FILE] [--phone-ppm 300] [--ring-ppm 0] [--seconds S] [--no-play]

Start it first, then QEMU. Without --wav it sends a test programme at 44.1 kHz: a sweep 20 Hz - 20 kHz, chords, and
a 1 kHz tone at the end, measured in 0.1 s windows. Standard library only.
"""
import argparse
import array
import math
import os
import random
import socket
import struct
import sys
import threading
import time
import wave

RING = 48000                    # the ring's rate
PULL = 480                      # frames per request from the ring: 10 ms
BURST = 512                     # frames per A2DP burst
JITTER = 0.010                  # +- s on every burst
HOLD = 0.060                    # a hold-up every 10 s, then the backlog at once
PORT = 5555
MSG_IN = struct.Struct('<B3xI')
MSG_OUT = struct.Struct('<BBHIff')
HERE = os.path.dirname(os.path.abspath(__file__))


def programme(rate):
    """The test programme as 16-bit stereo frames, and the second the 1 kHz tone starts at."""
    out = array.array('h')

    def put(v):
        s = int(round(v * 32767))
        out.append(s)
        out.append(s)

    a = 0.5                                         # -6 dBFS
    k = math.log(20000 / 20)
    for i in range(10 * rate):                      # exponential sweep, 10 s
        put(a * math.sin(2 * math.pi * 20 * 10 / k * (math.exp(k * i / rate / 10) - 1)))
    chords = ((261.63, 329.63, 392.00), (196.00, 246.94, 293.66), (220.00, 261.63, 329.63), (174.61, 220.00, 261.63))
    for notes in chords:                            # C, G, Am, F: struck, two overtones, decaying
        for i in range(2 * rate):
            t = i / rate
            env = 0.12 * math.exp(-t / 0.8)
            put(sum(env * (math.sin(2 * math.pi * f * t) + 0.5 * math.sin(4 * math.pi * f * t) +
                           0.25 * math.sin(6 * math.pi * f * t)) for f in notes))
    tone_at = len(out) / 2 / rate
    for i in range(6 * rate):
        put(a * math.sin(2 * math.pi * 1000 * i / rate))
    return out.tobytes(), tone_at


def load_wav(path):
    with wave.open(path, 'rb') as w:
        if w.getsampwidth() != 2:
            sys.exit(f'{path}: 16-bit PCM only')
        ch, rate, data = w.getnchannels(), w.getframerate(), w.readframes(w.getnframes())
    x = array.array('h', data)
    if ch == 1:
        y = array.array('h', bytes(4 * len(x)))
        y[0::2] = x
        y[1::2] = x
        x = y
    elif ch > 2:
        y = array.array('h', bytes(4 * (len(x) // ch)))
        y[0::2] = x[0::ch]
        y[1::2] = x[1::ch]
        x = y
    return x.tobytes(), rate


def recv_all(c, n):
    b = bytearray()
    while len(b) < n:
        k = c.recv(n - len(b))
        if not k:
            sys.exit('the firmware closed the connection')
        b += k
    return bytes(b)


def schedule(pcm, rate, phone_ppm, ring_ppm, seconds):
    """Both clocks in simulated time: the messages in the order the firmware would see them, and the request times."""
    n_in = len(pcm) // 4
    fp, fo = rate * (1 + phone_ppm * 1e-6), RING * (1 + ring_ppm * 1e-6)
    rnd = random.Random(1)
    msgs, times = [MSG_IN.pack(ord('R'), rate)], []
    k, t_in, j, total = 0, 0.0, 0, int(seconds * RING)
    while j * PULL < total:
        t_out = j * PULL / fo
        if k * BURST < n_in and t_in <= t_out:
            chunk = pcm[4 * k * BURST:4 * (k + 1) * BURST]
            msgs.append(MSG_IN.pack(ord('I'), len(chunk) // 4) + chunk)
            k += 1
            nom = k * BURST / fp
            hold = nom % 10
            t_in = nom - hold + HOLD if hold < HOLD else nom + rnd.uniform(-JITTER, JITTER)
        else:
            msgs.append(MSG_IN.pack(ord('P'), PULL))
            times.append(t_out)
            j += 1
    msgs.append(MSG_IN.pack(ord('Q'), 0))
    return msgs, times


def run(c, pcm, rate, phone_ppm, ring_ppm, seconds):
    """Sends the whole schedule from a second thread while this one reads the answers: nothing waits for a round trip
    over QEMU's network. Returns the 32-bit frames that came back and a log per request."""
    msgs, times = schedule(pcm, rate, phone_ppm, ring_ppm, seconds)
    writer = threading.Thread(target=lambda: [c.sendall(m) for m in msgs], daemon=True)
    writer.start()
    out, log = bytearray(), []
    for j, t_out in enumerate(times):
        _, playing, under, n, fill, corr = MSG_OUT.unpack(recv_all(c, MSG_OUT.size))
        out += recv_all(c, 8 * n)
        log.append((t_out, playing, under, fill, corr))
        if (j + 1) % 500 == 0:
            print(f'  {t_out:5.1f} s  fill {fill:6.1f} ms  correction {corr:+7.1f} ppm', flush=True)
    writer.join()
    return array.array('i', bytes(out)), log


def write_wav(path, frames):
    """16-bit for the sound card: the 32-bit slots rounded."""
    y = array.array('h', (max(-32768, min(32767, (v + 32768) >> 16)) for v in frames))
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RING)
        w.writeframes(y.tobytes())


def tone_snr(x, f0):
    """SNR of a sine near f0 Hz in x (floats at 48 kHz): the frequency that leaves the least residual after an exact
    least-squares fit of cos, sin and a constant, searched within +-1 Hz; noise and distortion are that residual.
    (Maximizing the plain projection instead is biased by the sine's mirror at -f0 in a short window.)"""
    n, sx, sxx = len(x), sum(x), sum(v * v for v in x)

    def fit(f):
        w = 2 * math.pi * f / RING
        cw, sw, c, s = math.cos(w), math.sin(w), 1.0, 0.0
        scc = sss = scs = sc = ss = sxc = sxs = 0.0
        for v in x:
            scc += c * c
            sss += s * s
            scs += c * s
            sc += c
            ss += s
            sxc += v * c
            sxs += v * s
            c, s = c * cw - s * sw, s * cw + c * sw
        m = [[scc, scs, sc], [scs, sss, ss], [sc, ss, float(n)]]
        r = [sxc, sxs, sx]
        for i in range(3):                          # Gauss-Jordan, 3 x 3
            piv = m[i][i]
            m[i] = [q / piv for q in m[i]]
            r[i] /= piv
            for k in range(3):
                if k != i:
                    g = m[k][i]
                    m[k] = [p - g * q for p, q in zip(m[k], m[i])]
                    r[k] -= g * r[i]
        sig = r[0] * sxc + r[1] * sxs + r[2] * sx   # energy of the fitted part
        return sxx - sig, sig

    lo, hi, g = f0 - 1.0, f0 + 1.0, (math.sqrt(5) - 1) / 2
    a, b = hi - g * (hi - lo), lo + g * (hi - lo)
    ea, eb = fit(a)[0], fit(b)[0]
    for _ in range(30):
        if ea < eb:
            hi, b, eb = b, a, ea
            a = hi - g * (hi - lo)
            ea = fit(a)[0]
        else:
            lo, a, ea = a, b, eb
            b = lo + g * (hi - lo)
            eb = fit(b)[0]
    err, sig = fit((lo + hi) / 2)
    return 10 * math.log10(sig / err) if err > 0 else 200.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--wav', help='16-bit WAV to send as the phone (default: the test programme)')
    ap.add_argument('--phone-ppm', type=float, default=300, help="the phone's clock offset (default +300)")
    ap.add_argument('--ring-ppm', type=float, default=0, help="the ring's clock offset (default 0)")
    ap.add_argument('--seconds', type=float, help='how long (default: the input, repeated if longer)')
    ap.add_argument('--out', default=os.path.join(HERE, '..', 'build_qemu'), help='folder for the results')
    ap.add_argument('--no-play', action='store_true', help='do not play the result')
    o = ap.parse_args()

    pcm, rate = load_wav(o.wav) if o.wav else (None, 44100)
    tone_at = None
    if pcm is None:
        print('making the test programme...', flush=True)
        pcm, tone_at = programme(rate)
    seconds = o.seconds or len(pcm) / 4 / rate
    while len(pcm) / 4 / rate < seconds:
        pcm += pcm
    srv = socket.create_server(('127.0.0.1', PORT))
    srv.settimeout(300)
    print(f'waiting for the firmware in QEMU on port {PORT}...', flush=True)
    c, _ = srv.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f'connected: phone {rate} Hz {o.phone_ppm:+.0f} ppm, ring {RING} Hz {o.ring_ppm:+.0f} ppm, {seconds:.1f} s',
          flush=True)
    began = time.monotonic()
    frames, log = run(c, pcm, rate, o.phone_ppm, o.ring_ppm, seconds)
    c.close()
    print(f'  {seconds:.1f} s of sound through the firmware in {time.monotonic() - began:.0f} s', flush=True)

    os.makedirs(o.out, exist_ok=True)
    wav_path, csv_path = os.path.join(o.out, 'vphone_out.wav'), os.path.join(o.out, 'vphone_log.csv')
    write_wav(wav_path, frames)
    with open(csv_path, 'w') as f:
        f.write('t_s,playing,underruns,fill_ms,corr_ppm\n')
        f.writelines(f'{t:.3f},{p},{u},{fl:.2f},{cr:.2f}\n' for t, p, u, fl, cr in log)

    start = next((e for e in log if e[1]), None)
    print('\nresult')
    if not start:
        print('  never played: the FIFO did not fill')
        return 1
    clocks = ((1 + o.phone_ppm * 1e-6) / (1 + o.ring_ppm * 1e-6) - 1) * 1e6
    late = [e for e in log if e[0] >= start[0] + 10] or log[-1:]
    print(f'  playing from {start[0]:.2f} s; underruns after that: {log[-1][2] - start[2]}')
    print(f'  correction {log[-1][4]:+.1f} ppm at the end, the clocks differ by {clocks:+.1f} ppm; '
          f'from 10 s after the start {min(e[4] for e in late):+.1f} .. {max(e[4] for e in late):+.1f} ppm')
    print(f'  fill from 10 s after the start {min(e[3] for e in late):.1f} .. {max(e[3] for e in late):.1f} ms '
          f'(target 100)')
    if tone_at is not None:
        t0 = tone_at / (1 + o.phone_ppm * 1e-6) + 0.12 + 1.0     # the tone in the output, settled
        t1 = seconds - 0.3
        f0 = 1000 * (1 + o.phone_ppm * 1e-6) / (1 + o.ring_ppm * 1e-6)
        snrs = []
        for k in range(8):
            i = int((t0 + k * (t1 - t0 - 0.1) / 7) * RING)
            snrs.append(tone_snr([frames[2 * (i + m)] / 2147483648.0 for m in range(RING // 10)], f0))
        snrs.sort()
        print(f'  1 kHz tone at -9 dBFS (-6 sent, -3 headroom), 8 windows of 0.1 s: SNR {snrs[4]:.1f} dB, '
              f'worst {snrs[0]:.1f} dB (16-bit input allows about 92)')
    print(f'  written: {os.path.abspath(wav_path)}\n           {os.path.abspath(csv_path)}', flush=True)
    if not o.no_play and sys.platform == 'win32':
        import winsound
        print("  playing it on the PC's sound card...", flush=True)
        winsound.PlaySound(wav_path, winsound.SND_FILENAME)
    return 0


if __name__ == '__main__':
    sys.exit(main())
