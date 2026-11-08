#!/usr/bin/env python3
"""run.py - the firmware's tests that run on the PC, in seconds, with no ESP32 and no QEMU (docs/firmware.md, "Перевірка").

QEMU has no OS8104A, so what depends on the chip's answers never runs there. Here the real modules are built with a
native compiler against small stand-ins (stubs/):
  test_power     power.c's task on a clock of the test's own: every timer on even and odd ticks and across the clock's wrap
  test_selftest  the firmware's selftests: the emulation, the sound processing, the phone's path
  test_alac      the ALAC decoder with guard areas: refused parameters, valid frames, a long run of damaged packets

    python run.py [--toolchain DIR_WITH_GCC] [--iterations N] [-v]

Exit code 0 only when all pass. Writes only into fw/build_host.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent.parent
MAIN = FW / 'main'
ALAC = FW / 'components' / 'alac'
OUT = FW / 'build_host'


def toolchain(given):
    for d in ([given] if given else []) + [os.environ.get('MINGW_BIN'), r'C:\Users\geniy\toolchains\mingw64\bin']:
        if d and (Path(d) / 'gcc.exe').is_file():
            return str(d)
    found = shutil.which('gcc')
    if found:
        return str(Path(found).parent)
    sys.exit('no gcc: give --toolchain DIR')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--toolchain')
    p.add_argument('--iterations', type=int, default=100000)
    p.add_argument('-v', action='store_true', help='print every line of the tests')
    o = p.parse_args()
    # the selftests' reference is usb2most's firmware itself: the header must still be what it gives
    if subprocess.run([sys.executable, str(HERE / 'golden.py'), '--check']).returncode:
        sys.exit('u2m_golden.h is not what ref/usb2most/fw/main-v24.bin gives: run golden.py')
    tc = toolchain(o.toolchain)
    env = dict(os.environ, PATH=tc + os.pathsep + os.environ['PATH'])
    OUT.mkdir(exist_ok=True)

    def build(*argv):
        r = subprocess.run(argv, cwd=HERE, env=env, capture_output=True, text=True)
        if r.returncode:
            print(' '.join(str(a) for a in argv))
            print(r.stdout + r.stderr)
            sys.exit('the build failed')

    c = [str(Path(tc) / 'gcc'), '-std=gnu11', '-O2', '-w', '-I', 'stubs', '-I', str(MAIN)]
    cxx = [str(Path(tc) / 'g++'), '-std=gnu++17', '-O2', '-w', '-DTARGET_RT_LITTLE_ENDIAN=1', '-I', str(ALAC)]
    build(*c, 'test_power.c', '-o', OUT / 'test_power.exe')
    build(*c, 'test_selftest.c', *[MAIN / f'{m}.c' for m in ('selftest', 'amp', 'dsp', 'most', 'music', 'phone', 'src')],
          '-lm', '-o', OUT / 'test_selftest.exe')
    objs = []
    for m in ('ALACBitUtilities', 'EndianPortable', 'ag_dec', 'dp_dec', 'matrix_dec'):
        obj = OUT / f'{m}.o'
        build(*c, '-DTARGET_RT_LITTLE_ENDIAN=1', '-I', str(ALAC), '-c', ALAC / f'{m}.c', '-o', obj)
        objs.append(obj)
    build(*cxx, '-include', str(HERE / 'guard.h'), '-c', ALAC / 'ALACDecoder.cpp', '-o', OUT / 'ALACDecoder.o')
    build(*cxx, 'test_alac.cpp', ALAC / 'alac_glue.cpp', OUT / 'ALACDecoder.o', *objs, '-o', OUT / 'test_alac.exe')

    bad = 0
    for name, args in (('test_power', []), ('test_selftest', []), ('test_alac', [str(o.iterations)])):
        t0 = time.time()
        r = subprocess.run([str(OUT / f'{name}.exe'), *args], env=env, capture_output=True, text=True, errors='replace')
        lines = r.stdout.splitlines()
        for line in lines:
            if o.v or 'FAIL' in line or ' passed, ' in line or ': ' in line and name != 'test_selftest':
                print(line)
        print(f'{name}: {"ok" if r.returncode == 0 else "FAILED, exit " + str(r.returncode)} in {time.time() - t0:.1f} s', flush=True)
        bad += r.returncode != 0
    print('all passed' if not bad else f'{bad} of 3 failed')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
