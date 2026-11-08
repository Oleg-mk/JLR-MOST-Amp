#!/usr/bin/env python3
"""vraop.py - an iPhone's AirPlay 1 for the firmware in QEMU (docs/firmware.md, "AirPlay").

Does what an iPhone does to an AirPlay 1 receiver, over QEMU's forwarded ports: RTSP with an Apple-Challenge (the
answer is checked against the AirPort key's public half), the stream's AES key wrapped in RSA, ALAC frames encrypted
with AES-128-CBC sent as RTP in real time, and resends of what the receiver asks for - every 150th packet is held back
on purpose. At the same time it is the ring, pulling 48 kHz frames as vphone.py does; then it writes the result,
reports and plays it on the PC's sound card. The ALAC frames are uncompressed ones (ALAC's escape frames), which the
receiver decodes like any other.

Run it with the ESP-IDF Python (it has the cryptography package), then QEMU with the forwarded ports:
    python vraop.py [--wav FILE] [--phone-ppm 300] [--volume -6] [--no-play]
    idf.py -B build_qemu qemu --qemu-extra-args "-nic user,model=open_eth,hostfwd=tcp::5000-:5000,hostfwd=udp::6000-:6000,hostfwd=udp::6001-:6001"
"""
import argparse
import array
import base64
import os
import random
import re
import socket
import struct
import sys
import threading
import time

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vphone as vp  # noqa: E402  the programme, the ring's protocol, the analysis

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = 352                    # per ALAC packet, as iPhones send
GUEST = '10.0.2.15'             # the firmware's address in QEMU's user network
RTSP, AUDIO, CTRL = 5000, 6000, 6001
DROP_EVERY = 150


def airport_key():
    """The AirPort Express key as the firmware carries it (main/raop_key.c)."""
    src = open(os.path.join(HERE, '..', 'main', 'raop_key.c'), encoding='utf-8').read()
    pem = ''.join(re.findall(r'"([^"]*)"', src.split('RAOP_KEY[] =')[1])).replace('\\n', '\n')
    return serialization.load_pem_private_key(pem.encode(), password=None)


def alac_escape(frames):
    """One uncompressed ALAC frame of 16-bit stereo: CPE tag, instance 0, 12 unused bits, header nibble (partial frame,
    no shift, escape), [32-bit frame count], the samples left and right, END tag, byte-aligned (ALACDecoder.cpp)."""
    n = len(frames) // 2
    partial = n != FRAMES
    bits, acc = 0, 0

    def put(v, k):
        nonlocal bits, acc
        acc = (acc << k) | (v & ((1 << k) - 1))
        bits += k
    put(1, 3)
    put(0, 4)
    put(0, 12)
    put((8 if partial else 0) | 1, 4)
    if partial:
        put(n, 32)
    be = array.array('h', frames)
    if sys.byteorder == 'little':
        be.byteswap()
    acc = (acc << (32 * n)) | int.from_bytes(be.tobytes(), 'big')
    bits += 32 * n
    put(7, 3)
    pad = -bits % 8
    acc <<= pad
    bits += pad
    return acc.to_bytes(bits // 8, 'big')


class Rtsp:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=20)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.cseq = 0
        self.buf = b''

    def ask(self, method, headers=(), body=b''):
        self.cseq += 1
        h = [f'{method} rtsp://{GUEST}/1 RTSP/1.0', f'CSeq: {self.cseq}', 'User-Agent: vraop/1', *headers]
        if body:
            h.append(f'Content-Length: {len(body)}')
        self.s.sendall(('\r\n'.join(h) + '\r\n\r\n').encode() + body)
        while b'\r\n\r\n' not in self.buf:
            k = self.s.recv(4096)
            if not k:
                sys.exit(f'{method}: the receiver closed the connection')
            self.buf += k
        head, self.buf = self.buf.split(b'\r\n\r\n', 1)
        lines = head.decode(errors='replace').split('\r\n')
        fields = {k.strip().lower(): v.strip() for k, v in (x.split(':', 1) for x in lines[1:] if ':' in x)}
        n = int(fields.get('content-length', 0))
        while len(self.buf) < n:
            self.buf += self.s.recv(4096)
        self.buf = self.buf[n:]
        return lines[0], fields


def check_response(resp, challenge, pub):
    """The Apple-Response: a raw PKCS#1 v1.5 signature over challenge, address, MAC, zero padding."""
    sig = int.from_bytes(base64.b64decode(resp + '=' * (-len(resp) % 4)), 'big')
    n = pub.public_numbers()
    m = pow(sig, n.e, n.n).to_bytes(256, 'big')
    sep = m.index(b'\x00', 2) if m[:2] == b'\x00\x01' else -1
    data = m[sep + 1:] if sep > 0 else b''
    ok = len(data) == 32 and data[:16] == challenge and data[16:20] == socket.inet_aton(GUEST)
    return ok, data[20:26].hex(':') if len(data) >= 26 else '?'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--wav', help='16-bit 44.1 kHz WAV to send (default: the test programme)')
    ap.add_argument('--phone-ppm', type=float, default=300, help="the iPhone's clock offset (default +300)")
    ap.add_argument('--volume', type=float, default=0.0, help="AirPlay volume, -30..0 (default 0)")
    ap.add_argument('--out', default=os.path.join(HERE, '..', 'build_qemu'), help='folder for the results')
    ap.add_argument('--no-play', action='store_true')
    o = ap.parse_args()
    if o.wav:
        pcm, rate = vp.load_wav(o.wav)
        if rate != 44100:
            sys.exit('AirPlay 1 carries 44.1 kHz: convert the file first')
        tone_at = None
    else:
        print('making the test programme...', flush=True)
        pcm, tone_at = vp.programme(44100)
    samples = array.array('h', pcm)
    seconds = len(samples) / 2 / 44100
    key = airport_key()
    pub = key.public_key()

    # the ring's side first: the firmware connects to it on its own
    srv = socket.create_server(('127.0.0.1', vp.PORT))
    srv.settimeout(300)
    print(f'waiting for the firmware in QEMU (ring on port {vp.PORT})...', flush=True)
    ring, _ = srv.accept()
    ring.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    for _ in range(60):
        try:
            rtsp = Rtsp('127.0.0.1', RTSP)
            break
        except OSError:
            time.sleep(1)
    else:
        sys.exit('no RTSP on port 5000: is QEMU running with the forwarded ports?')
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctrl.bind(('127.0.0.1', 0))
    timing = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    timing.bind(('127.0.0.1', 0))
    audio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    challenge = os.urandom(16)
    status, h = rtsp.ask('OPTIONS', [f"Apple-Challenge: {base64.b64encode(challenge).decode().rstrip('=')}"])
    ok, mac = check_response(h.get('apple-response', ''), challenge, pub)
    print(f'OPTIONS: {status}; Apple-Response {"verified" if ok else "WRONG"} (address {GUEST}, MAC {mac})')
    aes_key, aes_iv = os.urandom(16), os.urandom(16)
    wrapped = pub.encrypt(aes_key, padding.OAEP(mgf=padding.MGF1(hashes.SHA1()), algorithm=hashes.SHA1(), label=None))
    sdp = ('v=0\r\no=iTunes 1 0 IN IP4 10.0.2.2\r\ns=iTunes\r\nc=IN IP4 10.0.2.15\r\nt=0 0\r\n'
           'm=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n'
           'a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n'
           f"a=rsaaeskey:{base64.b64encode(wrapped).decode().rstrip('=')}\r\n"
           f"a=aesiv:{base64.b64encode(aes_iv).decode().rstrip('=')}\r\n")
    print('ANNOUNCE:', rtsp.ask('ANNOUNCE', ['Content-Type: application/sdp'], sdp.encode())[0])
    status, h = rtsp.ask('SETUP', [f'Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;'
                                   f'control_port={ctrl.getsockname()[1]};timing_port={timing.getsockname()[1]}'])
    print('SETUP:', status, '|', h.get('transport', ''))
    seq0 = random.randrange(65536)
    print('RECORD:', rtsp.ask('RECORD', ['Range: npt=0-', f'RTP-Info: seq={seq0};rtptime=0'])[0])
    vol = f'volume: {o.volume:.6f}\r\n'.encode()
    print('SET_PARAMETER volume:', rtsp.ask('SET_PARAMETER', ['Content-Type: text/parameters'], vol)[0], flush=True)

    sent, held, resends = {}, 0, 0
    lock = threading.Lock()

    def control():
        nonlocal resends
        ctrl.settimeout(0.5)
        while not done.is_set():
            try:
                b, _ = ctrl.recvfrom(64)
            except socket.timeout:
                continue
            if len(b) >= 8 and b[1] == 0xD5:
                first, count = struct.unpack('>HH', b[4:8])
                for i in range(count):
                    with lock:
                        p = sent.get((first + i) & 0xFFFF)
                    if p:
                        audio.sendto(b'\x80\xd6\x00\x01' + p, ('127.0.0.1', CTRL))
                        resends += 1

    out, log = bytearray(), []

    def reader():
        for _ in range(total_pulls):
            _, playing, under, n, fill, corr = vp.MSG_OUT.unpack(vp.recv_all(ring, vp.MSG_OUT.size))
            out.extend(vp.recv_all(ring, 8 * n))
            log.append((time.perf_counter() - t0, playing, under, fill, corr))

    done = threading.Event()
    n_pk = len(samples) // 2 // FRAMES
    total_pulls = int(seconds * vp.RING / vp.PULL)
    threading.Thread(target=control, daemon=True).start()
    t0 = time.perf_counter()
    rd = threading.Thread(target=reader, daemon=True)
    rd.start()
    fp = 44100 * (1 + o.phone_ppm * 1e-6)
    k = j = 0
    print(f'streaming {n_pk} packets ({seconds:.1f} s), every {DROP_EVERY}th held back...', flush=True)
    while k < n_pk or j < total_pulls:
        now = time.perf_counter() - t0
        t_pk, t_pull = k * FRAMES / fp, j * vp.PULL / vp.RING
        if k < n_pk and t_pk <= now:
            payload = alac_escape(samples[2 * FRAMES * k:2 * FRAMES * (k + 1)])
            whole = len(payload) & ~15
            enc = Cipher(algorithms.AES(aes_key), modes.CBC(aes_iv)).encryptor()
            payload = enc.update(payload[:whole]) + enc.finalize() + payload[whole:]
            seq = (seq0 + k) & 0xFFFF
            pkt = struct.pack('>BBHII', 0x80, 0xE0 if k == 0 else 0x60, seq, k * FRAMES, 0x4A4C5221) + payload
            with lock:
                sent[seq] = pkt
                sent.pop((seq - 400) & 0xFFFF, None)
            if k % DROP_EVERY == DROP_EVERY - 1:
                held += 1
            else:
                audio.sendto(pkt, ('127.0.0.1', AUDIO))
            k += 1
        elif j < total_pulls and t_pull <= now:
            ring.sendall(vp.MSG_IN.pack(ord('P'), vp.PULL))
            j += 1
        else:
            time.sleep(max(0.0, min(t_pk if k < n_pk else 1e9, t_pull if j < total_pulls else 1e9) - now))
    rd.join(timeout=30)
    done.set()
    print('TEARDOWN:', rtsp.ask('TEARDOWN')[0])
    ring.sendall(vp.MSG_IN.pack(ord('Q'), 0))
    ring.close()

    frames = array.array('i', bytes(out))
    os.makedirs(o.out, exist_ok=True)
    wav_path = os.path.join(o.out, 'vraop_out.wav')
    vp.write_wav(wav_path, frames)
    start = next((e for e in log if e[1]), None)
    print('\nresult')
    print(f'  packets held back {held}, resent on request {resends}')
    if not start:
        print('  never played')
        return 1
    late = [e for e in log if e[0] >= start[0] + 10] or log[-1:]
    print(f'  playing from {start[0]:.2f} s; underruns after that: {log[-1][2] - start[2]}')
    print(f'  correction {log[-1][4]:+.1f} ppm at the end (the iPhone is {o.phone_ppm:+.0f} ppm against the PC); '
          f'fill {min(e[3] for e in late):.1f} .. {max(e[3] for e in late):.1f} ms')
    if tone_at is not None:
        t0s = tone_at / (1 + o.phone_ppm * 1e-6) + start[0] + 1.0
        t1s = len(frames) / 2 / vp.RING - 0.3
        snrs = sorted(vp.tone_snr([frames[2 * (int((t0s + k * (t1s - t0s - 0.1) / 7) * vp.RING) + m)] / 2147483648.0
                                   for m in range(vp.RING // 10)], 1000 * (1 + o.phone_ppm * 1e-6)) for k in range(8))
        print(f'  1 kHz tone, 8 windows of 0.1 s: SNR {snrs[4]:.1f} dB, worst {snrs[0]:.1f} dB')
    print(f'  written: {os.path.abspath(wav_path)}', flush=True)
    if not o.no_play and sys.platform == 'win32':
        import winsound
        print("  playing it on the PC's sound card...", flush=True)
        winsound.PlaySound(wav_path, winsound.SND_FILENAME)
    return 0


if __name__ == '__main__':
    sys.exit(main())
