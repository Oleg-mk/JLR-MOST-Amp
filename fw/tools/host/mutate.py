#!/usr/bin/env python3
"""mutate.py - do the selftests catch the defects the check against usb2most found (docs/usb2most/check_amp.md)?

Each old defect goes back into a copy of amp.c, the selftests are built with it and run: they must fail. A test that stays
green on a defect tests nothing. Writes only into fw/build_host/mutate.

    python mutate.py [--toolchain DIR_WITH_GCC]

Exit code 0 only when all are caught.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent.parent
MAIN = FW / 'main'
OUT = FW / 'build_host' / 'mutate'

# what it is, then pairs: the text in amp.c, what the defect had there
MUT = [
    ('SpeakerLevel byte order swapped', '#define POS_ROW     0', '#define POS_ROW     1', '#define POS_SPK     1', '#define POS_SPK     0'),
    ('Hi-Line answers as Premium, 22.05', '{ 0x22, 0x02, pre_hi_amp, N(pre_hi_amp), pre_hi_amp_raw, sizeof pre_hi_amp_raw }', '{ 0x22, 0x05, pre_amp, N(pre_amp) }'),
    ('rows are SinkNrs on AUD3', '{ 4, 2, 2, 2 }, { 5, 0, 0, 0 },\n                                         { 6, 3, 2, 4 }, { 7, 4, 2, 3 }',
     '{ 4, 2, 2, 4 }, { 5, 0, 0, 0 },\n                                         { 6, 3, 2, 3 }, { 7, 4, 2, 2 }'),
    ('channels must ascend', 'if (d[2 + i] >= MAX_CH) {', 'if (d[2 + i] >= MAX_CH || (i && d[2 + i] <= d[1 + i])) {'),
    ('subscribes to 0161 whoever is the master', 'subscribe_gateway(m->src)', 'subscribe_gateway(0x161)'),
    ('a level above 0 dB refused', '    if (m->len < 2) return;\n    int row = d[POS_ROW]',
     '    if (m->len < 2) return;\n    for (int i = 2; i < m->len; i++) if ((int8_t)d[i] > 0) { error_par(m, MOST_ERR_PARAM, 2, d[i]); return; }\n    int row = d[POS_ROW]'),
    ('no clamp at 0 dB', '        if (v > 0) v = 0;\n', ''),
    ('no starting values of E0x', '    memcpy(s_e05, E05_INIT, sizeof s_e05);\n', ''),
    ('D22 of an unknown DID, the old error', 'error_info(m, MOST_ERR_PARAM, i, 3);\n}\n\n/* Diagnosis D19h', 'error_info(m, MOST_ERR_NOTAVAIL, i, 2);\n}\n\n/* Diagnosis D19h'),
    ('whole-table SpeakerLevel, row 1 only', 'for (int r = 1; r <= NROW && (r - 1) * P->nlvl < n; r++) {', 'for (int r = 1; r <= 1; r++) {'),
    ('Notification for a function not here: an error', '                ESP_LOGI(TAG, "notification: %04X for %03X: not here", dev, f);\n',
     '                { uint8_t b[3] = { 0x10, d[i], d[i + 1] }; error_info(m, MOST_ERR_FUNC, b, 3); }\n'),
    ('D00h not reported', '        d00_status(dev, 1);\n', ''),
    ('Diagnosis only under its own InstID', '|| m->fblock != AMP_FBLOCK) {', ') {'),
]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--toolchain', default=os.environ.get('MINGW_BIN') or r'C:\Users\geniy\toolchains\mingw64\bin')
    o = p.parse_args()
    env = dict(os.environ, PATH=o.toolchain + os.pathsep + os.environ['PATH'])
    OUT.mkdir(parents=True, exist_ok=True)
    src = (MAIN / 'amp.c').read_text(encoding='utf-8')
    others = [MAIN / f'{m}.c' for m in ('selftest', 'dsp', 'most', 'music', 'phone', 'src')]
    missed = 0
    for name, *pairs in MUT:
        s = src
        for a, b in zip(pairs[0::2], pairs[1::2]):
            if a not in s:
                sys.exit(f'{name}: the text is no longer in amp.c: {a[:60]!r}')
            s = s.replace(a, b, 1)
        path = OUT / 'amp.c'
        path.write_text(s, encoding='utf-8', newline='\n')
        exe = OUT / 't.exe'
        r = subprocess.run([str(Path(o.toolchain) / 'gcc'), '-std=gnu11', '-O1', '-w', '-I', 'stubs', '-I', str(MAIN),
                            'test_selftest.c', str(path), *map(str, others), '-lm', '-o', str(exe)],
                           cwd=HERE, env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'{name}: the build failed\n{r.stdout}{r.stderr}')
        t = subprocess.run([str(exe)], env=env, capture_output=True, text=True, errors='replace')
        fails = [ln.strip() for ln in t.stdout.splitlines() if ln.strip().startswith('FAIL')]
        missed += not fails
        print(f'{"caught" if fails else "MISSED"}  {name}: {len(fails)} checks fail' + (f', first: {fails[0][6:80]}' if fails else ''))
    shutil.rmtree(OUT, ignore_errors=True)
    print(f'{len(MUT) - missed} of {len(MUT)} caught')
    return 1 if missed else 0


if __name__ == '__main__':
    sys.exit(main())
