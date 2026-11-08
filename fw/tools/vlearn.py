#!/usr/bin/env python3
"""vlearn.py - the read mode (stage 7) for the firmware in QEMU (docs/firmware.md, "Серійний номер і навчання").

QEMU has no ring and no stock amplifier, so this script is both. It gives the firmware its place in the ring, sends
the network master's Configuration OK and answers the firmware's questions as a pre-facelift Hi-Line amplifier:
FBlocks, function lists and part numbers as usb2most keeps them (ref/usb2most/fw/main-v24.bin), a serial number of
its own. Then it ends the read mode and asks the emulation what a head unit asks: the answers must be the learned
amplifier's byte for byte - Hi-Line's 22.02 set, lists not ascending included, not the Premium set SW1 stands on.

    python vlearn.py            (start it first, then QEMU as for vphone.py)
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vphone as vp  # noqa: E402  the link's framing

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, '..', '..', 'ref', 'usb2most', 'fw', 'main-v24.bin')
POS, COUNT = 3, 6                   # this node's place in the virtual ring; the amplifier is at POS + 1
AMP, GW = 0x186, 0x161
SERIAL = b'LR2SN0042XYZ'            # made up: the read must bring this one, not the default zeros
MOST_DATA = 12                      # data bytes in one telegram


def usb2most():
    """The Hi-Line set: FBlocks, each FBlock's FktIDs Status, the Diagnosis answers."""
    b = open(FW, 'rb').read()
    fblocks = b[0x2E9FE:0x2EA06]                                    # 22.02 22.20 23.01 06.86
    fkts = {(0x22, 0x02): b[0x25AA6:0x25AC7], (0x22, 0x20): b[0x259CB:0x259D6],
            (0x23, 0x01): b[0x259C0:0x259CB], (0x06, 0x86): b[0x25AE7:0x25B0D]}
    dids = {0xF18C: SERIAL + bytes(4)}
    for off in (0x25A3E, 0x25A58, 0x25A72, 0x25A8C):                # F112 F191 F188 F124, 24-byte fields
        dids[b[off] << 8 | b[off + 1]] = b[off + 2:off + 26]
    return fblocks, fkts, dids


class Link:
    def __init__(self, c):
        self.c, self.parts = c, {}

    def send(self, typ, n, payload=b''):
        self.c.sendall(vp.MSG_IN.pack(ord(typ), n) + payload)

    def tele(self, src, fb, inst, fkt, op, data=b''):
        """A telegram to the firmware, in segments of 11 bytes after a counter when longer than 12."""
        if len(data) <= MOST_DATA:
            segs = [(0, data)]
        else:
            chunks = [data[i:i + 11] for i in range(0, len(data), 11)]
            segs = [(1 if i == 0 else 3 if i == len(chunks) - 1 else 2, bytes([i]) + ch) for i, ch in enumerate(chunks)]
        for tel, d in segs:
            t = struct.pack('>HBBHBB', src, fb, inst, fkt, op | tel << 4, len(d)) + d
            self.send('C', len(t), t)

    def recv(self, timeout):
        """The next whole message out of the firmware ('M', segments joined): (dst, fb, inst, fkt, op, data) or None."""
        self.c.settimeout(timeout)
        while True:
            try:
                typ, _, _, n, _, _ = vp.MSG_OUT.unpack(vp.recv_all(self.c, vp.MSG_OUT.size))
            except (socket.timeout, TimeoutError):
                return None
            if typ != ord('M'):
                vp.recv_all(self.c, 8 * n)
                continue
            t = vp.recv_all(self.c, n)
            dst, fb, inst, fkt, opt, ln = struct.unpack('>HBBHBB', t[:8])
            op, tel, d = opt & 15, opt >> 4, t[8:8 + ln]
            key = (dst, fb, inst, fkt, op)
            if tel == 0:
                return key + (d,)
            self.parts[key] = (b'' if tel == 1 else self.parts.get(key, b'')) + d[1:]
            if tel == 3:
                return key + (self.parts.pop(key),)



def main():
    fblocks, fkts, dids = usb2most()
    srv = socket.create_server(('127.0.0.1', vp.PORT))
    srv.settimeout(300)
    print(f'waiting for the firmware in QEMU on port {vp.PORT}...', flush=True)
    c, _ = srv.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    ln = Link(c)
    ln.send('W', 1)
    ln.send('T', 1)                                                 # SW1 on Premium: 22.05 until something is read
    ln.send('L', POS | COUNT << 8)
    ln.tele(GW, 0x02, 0x01, 0xA00, 0xC, b'\x01')                    # the master: network OK
    ln.tele(GW, 0x01, POS, 0x000, 0x1)                              # and its scan asks this node too
    print(f'read mode: this node at position {POS} of {COUNT}, the amplifier at {POS + 1}\n')

    fails, asked_dids, empty_ok = [], 0, False
    t_end = time.time() + 30
    while time.time() < t_end:
        m = ln.recv(3.0)
        if m is None:
            break
        dst, fb, inst, fkt, op, d = m
        print(f'  firmware -> {dst:04X} {fb:02X}.{inst:02X} {fkt:03X} op {op:X} [{d.hex(" ")}]')
        if dst == GW and fb == 0x01 and op == 0xC:
            empty_ok = len(d) == 0
        elif dst == 0x400 + POS + 1 and fb == 0x01 and fkt == 0x000 and op == 0x1:
            ln.tele(AMP, 0x01, inst, 0x000, 0xC, fblocks)
        elif dst == AMP and fkt == 0x000 and op == 0x1 and (fb, inst) in fkts:
            ln.tele(AMP, fb, inst, 0x000, 0xC, fkts[(fb, inst)])
        elif dst == AMP and (fb, inst, fkt, op) == (0x06, 0x86, 0xD22, 0x2) and len(d) == 2:
            did = d[0] << 8 | d[1]
            asked_dids += 1
            if did in dids:
                ln.tele(AMP, 0x06, 0x86, 0xD22, 0xC, d + dids[did])
            else:
                ln.tele(AMP, 0x06, 0x86, 0xD22, 0xF, bytes([0x07, 0x01, d[0]]))
        if asked_dids == 5:
            time.sleep(1.0)                                         # time to keep it in NVS
            break
    print(f'\nthe master\'s NetBlock Get to this node: {"empty FBlock list" if empty_ok else "NO empty answer"}')
    if not empty_ok:
        fails.append('empty FBlock list')
    if asked_dids != 5:
        fails.append(f'{asked_dids} of 5 Diagnosis questions')

    # the emulation now: what the head unit asks
    ln.send('T', 1)
    time.sleep(0.5)
    checks = [
        ('NetBlock FBlockIDs', (0x01, POS, 0x000, 0x1, b''), (0x01, POS, 0x000, 0xC, fblocks)),
        ('22.02 functions', (0x22, 0x02, 0x000, 0x1, b''), (0x22, 0x02, 0x000, 0xC, fkts[(0x22, 0x02)])),
        ('Diagnosis functions', (0x06, 0x86, 0x000, 0x1, b''), (0x06, 0x86, 0x000, 0xC, fkts[(0x06, 0x86)])),
        ('serial number', (0x06, 0x86, 0xD22, 0x2, b'\xf1\x8c'), (0x06, 0x86, 0xD22, 0xC, b'\xf1\x8c' + dids[0xF18C])),
        ('assembly part number', (0x06, 0x86, 0xD22, 0x2, b'\xf1\x12'),
         (0x06, 0x86, 0xD22, 0xC, b'\xf1\x12' + dids[0xF112])),
        ('22.05 (the set SW1 stands on) gone: no answer', (0x22, 0x05, 0x000, 0x1, b''), None),
    ]
    print('\nthe emulation after the read:')
    for name, (fb, inst, fkt, op, d), want in checks:
        ln.tele(GW, fb, inst, fkt, op, d)
        m = ln.recv(3.0)
        got = m[1:] if m else None
        ok = got == want
        print(f'  {"pass" if ok else "FAIL"}  {name}: {got[4].hex(" ") if got else "no answer"}')
        if not ok:
            fails.append(name)
    ln.send('Q', 0)
    c.close()
    print(f'\nresult: {"all passed" if not fails else "FAILED: " + ", ".join(fails)}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
