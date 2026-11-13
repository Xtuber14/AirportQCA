#!/usr/bin/env python3
"""Check that each child vtable is at least as long as its parent vtable.

OpenCore's InternalInitializeVtableByEntriesAndRelocations iterates
`Index < SuperVtable->NumEntries` while indexing the *child's* VtableData.
If the child's table is shorter, OpenCore reads past the end of the child's
vtable into adjacent data and mis-patches.  This must be verified.
"""
import re
import sys
import macholib
import kernel as kmod
import simulate

VTABLE_HEADER_LEN = 2


def child_entry_count(k, vname):
    slots = k.vtable_slots(vname)
    if slots is None:
        return None
    # slots ends with the terminator (raw==0, reloc==None)
    return len(slots) - 1


def main():
    path = sys.argv[1]
    k = simulate.Kext(path)
    kern = kmod.Kernel()

    def parent_count(vtable_name):
        ident, slots = kern.vtable(vtable_name)
        n = 0
        for raw_idx, name, addr in slots:
            if raw_idx < VTABLE_HEADER_LEN:
                continue
            if addr is None:
                break
            n += 1
        return n

    pairs = [
        ('__ZTV10AirportQCA', '__ZTV17IO80211Controller'),
        ('__ZTV19AirportQCAInterface', '__ZTV20IO80211InfraProtocol'),
        ('__ZTV27AirportQCAEthernetInterface', '__ZTV19IOEthernetInterface'),
        ('__ZTVN10AirportQCA9MetaClassE', '__ZTV11OSMetaClass'),
        ('__ZTVN19AirportQCAInterface9MetaClassE', '__ZTV11OSMetaClass'),
        ('__ZTVN27AirportQCAEthernetInterface9MetaClassE', '__ZTV11OSMetaClass'),
    ]

    bad = 0
    print('%-46s %8s %8s  %s' % ('child vtable', 'child', 'parent', 'verdict'))
    for child, parent in pairs:
        cc = child_entry_count(k, child)
        pc = parent_count(parent)
        if cc is None:
            verdict = 'CHILD MISSING'
            bad += 1
        elif cc < pc:
            verdict = 'TOO SHORT (OpenCore would read past end)'
            bad += 1
        elif cc == pc:
            verdict = 'exact'
        else:
            verdict = 'OK (child longer)'
        print('%-46s %8s %8s  %s' % (child, cc, pc, verdict))

    print()
    print('RESULT: %s' % ('ALL LENGTHS OK' if bad == 0 else '%d PROBLEMS' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
