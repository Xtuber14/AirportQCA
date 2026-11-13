#!/usr/bin/env python3
"""Check a kext binary's undefined symbols against the Boot KC symbol pool."""
import struct
import sys
import kclib
import macholib

KC = '/System/Library/KernelCollections/BootKernelExtensions.kc'


def main():
    path = sys.argv[1]
    data, top, entries = kclib.parse_fileset(KC)
    pool = set()
    for ident, off, vm in entries:
        try:
            m = kclib.entry_macho(data, off)
        except Exception:
            continue
        if not m.nsyms:
            continue
        for s in m.symbols():
            if s['type'] & 0xE0:      # stab
                continue
            if s['type'] & 0x01 and not (s['type'] & 0x0E):
                continue  # undefined
            pool.add(s['name'])
    km = macholib.load(path)
    undef = []
    for s in km.symbols():
        if s['type'] & 0x01 and not (s['type'] & 0x0E) and s['name']:
            undef.append(s['name'])
    missing = sorted(set(u for u in undef if u not in pool))
    print('undefined symbols: %d' % len(set(undef)))
    print('MISSING from KC: %d' % len(missing))
    for mname in missing:
        print('   ', mname)


if __name__ == '__main__':
    main()
