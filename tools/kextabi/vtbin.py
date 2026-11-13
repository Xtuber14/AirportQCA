#!/usr/bin/env python3
"""Dump a vtable of a linked kext binary (unified relocations)."""
import struct
import sys
import macholib


def main():
    path = sys.argv[1]
    vname = sys.argv[2]
    lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    hi = int(sys.argv[4]) if len(sys.argv) > 4 else 10 ** 6
    m = macholib.load(path)
    sym = m.find_symbol(vname)
    if sym is None:
        print('not found')
        return
    sec = m.section_for_addr(sym['value'])
    data = m.section_data(sec)
    urel = m.unified_relocs()
    srel = {}
    for r_addr, (kind, val) in m.relocs(sec).items():
        if kind == 'extern':
            srel[r_addr + sec.addr] = val
    local_by_addr = {}
    for s in m.symbols():
        if (s['type'] & 0x0E) and s['sect'] and s['value']:
            local_by_addr.setdefault(s['value'], s['name'])
    print('# %s at %s section %s,%s' % (vname, hex(sym['value']), sec.segname, sec.name))
    idx = 0
    base_off = sym['value'] - sec.addr
    while idx < hi:
        off = base_off + 8 * idx
        if off + 8 > len(data):
            break
        raw = struct.unpack_from('<Q', data, off)[0]
        addr = sym['value'] + 8 * idx
        name = urel.get(addr, srel.get(addr))
        if name is None and raw:
            name = local_by_addr.get(addr)
        if idx >= lo:
            if raw == 0 and name is None:
                print('[%3d] 0' % idx)
            else:
                print('[%3d] %s' % (idx, name if name else '?raw=%x' % raw))
        idx += 1


if __name__ == '__main__':
    main()
