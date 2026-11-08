"""Bitline Lite: the same board with fewer parts. A copy of a board project where the parts that are not fitted are marked DNP
in its schematic and on its board and lose their solder paste, and the rest get their LCSC numbers; copper, mask, holes and
silkscreen stay as they are. fab.py then makes its package:
    python -B lite.py ../../hw OUT/2-layer
    python.exe fab.py OUT/2-layer Lite-2L
    python -B lite.py ../../hw4 OUT/4-layer
    python.exe fab.py OUT/4-layer Lite-4L
Every board's J3 is DNP already: soldered by hand, in the full version too."""
import glob
import os
import re
import shutil
import sys
import uuid

NOT_FITTED = {
    'DAC': 'U3 U9 C30 C31 C32 C33 C34 C35 C36 C37 C38 C61 C62 C72 C73 C74 R30 R31',
    'RCA L/R, tube board': 'J4 J5 J8 R32 R33 C39 C40',
    'HDMI': 'J7 U6 C44 R13',
    'REM': 'U10 R57 R58 R59 C63 C77 J9',
    'USB': 'J10 U11 U12 U13 Q2 Q3 R60 R61 R62 R63 R64 R65 C66 C67 C68',
}
REFS = {r for v in NOT_FITTED.values() for r in v.split()}
assert len(REFS) == 51
SKIP = ('gen', 'production', 'variants', 'fp-info-cache')
# JLC catalog numbers, read there on 2026-11-06, against docs/board_check/check.md. A part without a number is one JLC does not
# place here: U1 (not sold) and the through-hole parts D20, J1, J2, J6, SW1, U5
LCSC = {
    'C14663': 'C1 C3 C5 C6 C8 C9 C15 C16 C17 C21 C41 C42 C43 C53 C57 C60 C64 C69 C70 C71 C75 C76',  # 100n 50V X7R
    'C15850': 'C2 C4 C7 C10',       # 10u 25V X5R 0805
    'C1647': 'C11 C12',             # 18p C0G, with Y1's 12 pF load
    'C105970': 'C13',               # 100n NP0 1206
    'C106248': 'C14 C22 C54',       # 1u 16V X7R: C54 must be X7R (LM656x5 7.3.6)
    'C12891': 'C20 C58 C59',        # 22u 25V X5R 1206
    'C165625': 'C50 C65',           # 47u 50V, 8x10, -40..+105
    'C28323': 'C51',                # 1u 50V X7R 0805
    'C19110': 'C52',                # 2.2u 25V X7R 0805
    'C138687': 'C55 C56',           # 10u 50V X7R 1210, Samsung CL32B106KBJNNNE (LM656x5 8.2.2.6)
    'C8678': 'D1',                  # SS34
    'C110904': 'D2',                # ST SM6T30A
    'C37704': 'D3',                 # Nexperia BAT54C,215, common cathode, SOT-23
    'C151112': 'F1',                # Littelfuse 043701.5WR, 63 V: D2's clamp is above a 32 V fuse's rating
    'C1002': 'FB1 FB2 FB3',         # 600R, 200 mA, 450 mOhm; FB2 carries at most 50 mA
    'C842750': 'L1',                # NRSE6028-100M: terminals 1.55 x 4.8, gap 2.9, on our 1.6 x 5.7 pads, gap 3.1
    'C5291938': 'L2',               # CKCSA4020-2.2uH/M
    'C8492': 'Q1',                  # LBSS84LT1G
    'C22775': 'R1 R5', 'C25803': 'R2 R17 R50 R51', 'C25819': 'R3 R7 R10 R11', 'C22809': 'R4', 'C4190': 'R6',
    'C23162': 'R8 R9 R27 R28', 'C25804': 'R12 R16 R20 R21 R22 R23 R24 R25 R52', 'C23182': 'R14 R15', 'C27592': 'R18',
    'C21190': 'R26', 'C22919': 'R34',
    'C172192': 'R35',               # Walsin 107R 1%: UNI-ROYAL C22778 is pre-order, 5000 pieces
    'C2886899': 'SW2',              # TL3305AF160QG
    'C701352': 'U2',
    'C176930': 'U4',                # Diodes 74LVC1G125W5-7, SOT-25
    'C2876250': 'U7',               # TPS7B8133DRVR
    'C49305977': 'U8',              # LM65625SRZTRQ1, pre-order
    'C362472': 'Y1',                # 12 pF, 60 Ohm, -40..+85, 3225: OS8104A table 20-1 wants 12-22 pF, at most 40 Ohm (none in 3225)
}
PART = {r: n for n, v in LCSC.items() for r in v.split()}
assert not set(PART) & REFS


def no_paste(blk):
    """The DNP flag does not reach the Gerbers: without this the stencil would still put paste on the empty pads."""
    while True:
        m = re.search(r'\(layers "[FB]\.Paste"\)', blk)             # pads that are paste only, under a thermal pad
        if not m:
            break
        start = blk.rfind('(pad ', 0, m.start())
        depth = 0
        for end in range(start, len(blk)):
            depth += {'(': 1, ')': -1}.get(blk[end], 0)
            if depth == 0:
                break
        while blk[start - 1] in ' \t\r\n':
            start -= 1
        blk = blk[:start] + blk[end + 1:]
    return re.sub(r' "[FB]\.Paste"', '', blk)


def main(src, out):
    pro = glob.glob(os.path.join(src, '*.kicad_pro'))
    assert len(pro) == 1, src
    base = os.path.basename(pro[0])[:-len('.kicad_pro')]
    if os.path.exists(out):                                     # only ever replaces an earlier copy of the same project
        assert os.path.exists(os.path.join(out, base + '.kicad_pro')), out + ' is something else'
        shutil.rmtree(out)
    os.makedirs(out)
    for f in os.listdir(src):
        p = os.path.join(src, f)
        if f in SKIP or f.startswith(('~', '.')) or f.endswith(('.kicad_prl', '-backups')):
            continue
        (shutil.copytree if os.path.isdir(p) else shutil.copyfile)(p, os.path.join(out, f))

    done, numbered = set(), set()
    for sheet in glob.glob(os.path.join(out, '*.kicad_sch')):
        t = open(sheet, encoding='utf-8', newline='').read()
        parts = re.split(r'(?=\(symbol\s*\(lib_id )', t)
        for i, blk in enumerate(parts):
            m = re.search(r'\(property "Reference" "([^"]+)"', blk)
            if m and m.group(1) in REFS:
                parts[i], n = re.subn(r'\(dnp no\)', '(dnp yes)', blk, count=1)
                assert n == 1, m.group(1)
                done.add(m.group(1))
            elif m and m.group(1) in PART:
                d = re.search(r'\(property "Datasheet" "[^"]*" \(at ([^)]*)\) \(effects \(font \(size 1\.27 1\.27\)\) \(hide yes\)\)\)', blk)
                assert d, m.group(1)
                field = ' (property "LCSC" "%s" (at %s) (effects (font (size 1.27 1.27)) (hide yes)))' % (PART[m.group(1)], d.group(1))
                parts[i] = blk[:d.end()] + field + blk[d.end():]
                numbered.add(m.group(1))
        open(sheet, 'w', encoding='utf-8', newline='').write(''.join(parts))
    assert done == REFS, REFS - done
    assert numbered == set(PART), set(PART) - numbered

    pcb = os.path.join(out, base + '.kicad_pcb')
    t = open(pcb, encoding='utf-8', newline='').read()
    parts = re.split(r'(?=\n\t\(footprint )', t)
    done, numbered = set(), set()
    for i, blk in enumerate(parts):
        m = re.search(r'\(property "Reference" "([^"]+)"', blk)
        if m and m.group(1) in REFS:
            blk, n = re.subn(r'\(attr (smd|through_hole)\)', r'(attr \1 dnp)', blk, count=1)
            assert n == 1, m.group(1)
            parts[i] = no_paste(blk)
            done.add(m.group(1))
        elif m and m.group(1) in PART:                          # the same field as in the schematic, or the board differs from it
            d = re.search(r'\(property "Datasheet" "[^"]*"(.*?)\(uuid "[^"]+"\)(.*?\n\t\t\))', blk, re.S)
            assert d, m.group(1)
            field = '(property "LCSC" "%s"%s(uuid "%s")%s' % (PART[m.group(1)], d.group(1), uuid.uuid5(uuid.NAMESPACE_URL, 'bitline-lite/' + m.group(1)), d.group(2))
            parts[i] = blk[:d.end()] + ('\r\n' if '\r\n' in blk else '\n') + '\t\t' + field + blk[d.end():]
            numbered.add(m.group(1))
    assert done == REFS, REFS - done
    assert numbered == set(PART), set(PART) - numbered
    open(pcb, 'w', encoding='utf-8', newline='').write(''.join(parts))
    print(out, ': %d parts marked not fitted' % len(REFS))


if __name__ == '__main__':
    main(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]))
