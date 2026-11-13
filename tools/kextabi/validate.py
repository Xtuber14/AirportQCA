#!/usr/bin/env python3
"""Full pre-boot validation suite for AirportQCA.kext.

Runs every check needed to predict OpenCore's prelinked-injection result:
  1. undefined symbols vs the Tahoe Boot KC
  2. vtable lengths (OpenCore reads the child table using the parent's count)
  3. vtable patch simulation (OpenCore's exact patch rules)
  4. pad-slot failure analysis (the "Invalid Parameter" root cause)
"""
import subprocess
import sys

HERE = __file__.rsplit('/', 1)[0]


def run(cmd):
    print('$ %s' % ' '.join(cmd))
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = (p.stdout or '') + (p.stderr or '')
    print(out.rstrip())
    print()
    return p.returncode


def main():
    kext = sys.argv[1]
    rc = 0

    print('=' * 72)
    print('1) undefined symbols vs Boot KC')
    print('=' * 72)
    if run(['python3', HERE + '/missing.py', kext]):
        rc = 1

    print('=' * 72)
    print('2) vtable lengths')
    print('=' * 72)
    if run(['python3', HERE + '/vtlen.py', kext]):
        rc = 1

    print('=' * 72)
    print('3) vtable patch simulation')
    print('=' * 72)
    if run(['python3', HERE + '/simulate.py', kext]):
        rc = 1

    print('=' * 72)
    print('4) pad-slot failure analysis')
    print('=' * 72)
    if run(['python3', HERE + '/padcheck.py', kext]):
        rc = 1

    print('=' * 72)
    print('5) patch-rule unit tests')
    print('=' * 72)
    if run(['python3', HERE + '/test_simulate.py']):
        rc = 1

    print('=' * 72)
    print('OVERALL: %s' % ('PASS' if rc == 0 else 'FAIL'))
    print('=' * 72)
    return rc


if __name__ == '__main__':
    sys.exit(main())
