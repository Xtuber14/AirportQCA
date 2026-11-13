#!/usr/bin/env python3
"""Decode a vtable from the Tahoe Boot KC.

Handles both fixup-encoded kext entries and the kernel's own pre-resolved
absolute addresses, mirroring OpenCore's KcFixupValue().
"""
import struct
import sys
import kclib

KC = '/System/Library/KernelCollections/BootKernelExtensions.kc'
KERNEL_ADDRESS_MASK = 0xFFFFFFFF00000000
KERNEL_ADDRESS_BASE = 0xFFFFFF8000000000
KERNEL_ADDRESS_KEXT = 0xFFFFFF7F00000000
KERNEL_FIXUP_OFFSET = 0x100000


def kc_fixup(value):
    """Mirror of OpenCore's KcFixupValue()."""
    if ((value & KERNEL_ADDRESS_MASK) != KERNEL_ADDRESS_BASE
            and (value & KERNEL_ADDRESS_MASK) != KERNEL_ADDRESS_KEXT):
        target = value & 0x3FFFFFFF
        cache = (value >> 30) & 3
        diversity = (value >> 32) & 0xFFFF
        if cache or diversity:
            return None  # not a plain rebase; cannot resolve
        value = target + KERNEL_FIXUP_OFFSET + KERNEL_ADDRESS_BASE
    return value


def build_symmap(data, entries):
    symmap = {}
    for ident, off, vm in entries:
        try:
            m = kclib.entry_macho(data, off)
        except Exception:
            continue
        if not m.nsyms:
            continue
        for s in m.symbols():
            if (s['type'] & 0x0E) and s['sect'] and s['value'] and s['name']:
                symmap.setdefault(s['value'], s['name'])
    return symmap


def main():
    vtable_name = sys.argv[1]
    nslots = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    data, top, entries = kclib.parse_fileset(KC)
    symmap = build_symmap(data, entries)

    found = None
    for ident, off, vm in entries:
        try:
            m = kclib.entry_macho(data, off)
        except Exception:
            continue
        sym = m.find_symbol(vtable_name)
        if sym and sym['value']:
            sec = None
            for s in m.segs:
                for sc in s.sects:
                    if sc.size and sc.addr <= sym['value'] < sc.addr + sc.size:
                        sec = sc
            if sec is None:
                continue
            found = (ident, m, sym, sec)
            break
    if not found:
        print('vtable not found')
        return
    ident, m, sym, sec = found
    fo = sec.offset + (sym['value'] - sec.addr)

    nexts = [s['value'] for s in m.symbols()
             if s['value'] > sym['value'] and s['value'] < sec.addr + sec.size]
    end_addr = min(nexts) if nexts else sec.addr + sec.size
    total = (end_addr - sym['value']) // 8

    print('# %s in %s at %s (file %s)' % (vtable_name, ident, hex(sym['value']), hex(fo)))
    limit = min(nslots, total) if nslots else total
    for idx in range(limit):
        raw = struct.unpack_from('<Q', data, fo + 8 * idx)[0]
        if raw == 0:
            print('[%3d] 0' % idx)
            continue
        addr = kc_fixup(raw)
        if addr is None:
            print('[%3d] ?raw=%016x' % (idx, raw))
            continue
        name = symmap.get(addr)
        print('[%3d] %s' % (idx, name if name else '?@%s' % hex(addr)))
    print('# total entries %d' % total)


if __name__ == '__main__':
    main()
