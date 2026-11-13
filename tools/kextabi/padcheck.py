#!/usr/bin/env python3
"""Definitive pad-slot failure analysis for a candidate kext.

OpenCore aborts a vtable patch (case 4 of InternalPatchVtableSymbol) iff the
*resolved* parent entry name differs from the child's vtable relocation symbol
name AND contains "_RESERVED".

This script evaluates that condition for every slot of all 6 vtables of
AirportQCA:

  parent entry name resolution (in OpenCore's order):
    1. the owning kext's own C++ symbol table, with OpenCore's exact
       "last symbol in symtab order wins" ordering  -> authoritative
    2. otherwise the dependency tree, modelled by the global KC map

  child slot state:
    - non-zero raw value  -> locally defined -> case 1 -> skip (safe)
    - zero + relocation   -> child symbol name
    - zero, no relocation -> end of table
"""
import sys
import kclib
import kernel as kmod
import simulate

KC = kmod.KC
VTABLE_HEADER_LEN = 2
RESERVED = '_RESERVED'

PAIRS = [
    ('__ZTV10AirportQCA', '__ZTV17IO80211Controller'),
    ('__ZTV19AirportQCAInterface', '__ZTV20IO80211InfraProtocol'),
    ('__ZTV27AirportQCAEthernetInterface', '__ZTV19IOEthernetInterface'),
    ('__ZTVN10AirportQCA9MetaClassE', '__ZTV11OSMetaClass'),
    ('__ZTVN19AirportQCAInterface9MetaClassE', '__ZTV11OSMetaClass'),
    ('__ZTVN27AirportQCAEthernetInterface9MetaClassE', '__ZTV11OSMetaClass'),
]


def main():
    kext_path = sys.argv[1]
    k = simulate.Kext(kext_path)
    kern = kmod.Kernel()

    stats = {'local': 0, 'same': 0, 'patch': 0, 'parent_null': 0,
             'FAIL': 0, 'alias_ok': 0, 'alias_risk': 0}
    failures = []

    for child_vt, parent_vt in PAIRS:
        ident, slots = kern.vtable(parent_vt)
        cslots = k.vtable_slots(child_vt)

        for raw_idx, pname, addr in slots:
            if raw_idx < VTABLE_HEADER_LEN:
                continue
            if addr is None:
                break
            idx = raw_idx - VTABLE_HEADER_LEN

            # --- child slot state ---
            if idx >= len(cslots) - 1:
                continue
            child_raw, child_reloc = cslots[idx]

            if child_raw != 0:
                stats['local'] += 1          # case 1: locally defined -> skip
                continue
            if child_reloc is None:
                stats['parent_null'] += 1
                continue
            child_name = child_reloc

            # --- parent entry name ---
            if pname is not None:
                # Authoritative: OpenCore resolved it through the owning
                # kext's own table.
                if pname == child_name:
                    stats['same'] += 1
                    continue
                if RESERVED in pname:
                    stats['FAIL'] += 1
                    failures.append((child_vt, idx, child_name, [pname]))
                    continue
                stats['patch'] += 1           # case 6: harmless rebind
                continue

            # Not resolvable in the owning kext: OpenCore would continue
            # through the dependency tree.
            candidates = kern.lookup_candidates(addr)
            if not candidates:
                stats['parent_null'] += 1   # parent entry has no symbol -> skip
                continue
            if child_name in candidates:
                # Case 3 would apply if the dependency lookup yields the same
                # name.  Check whether a pad-slot alias could win instead.
                aliases = sorted(n for n in candidates
                                 if n != child_name and RESERVED in n)
                if not aliases:
                    stats['same'] += 1
                    continue
                # Resolve the alias the same way OpenCore would: per entry,
                # using the own-entry symbol table ordering.
                per_entry = kern.resolve_per_entry(addr)
                winners = set(per_entry.values())
                if winners and all(w == child_name for w in winners):
                    stats['alias_ok'] += 1
                    continue
                stats['alias_risk'] += 1
                failures.append((child_vt, idx, child_name,
                                 aliases + sorted(winners)))
                continue
            reserved = sorted(n for n in candidates if RESERVED in n)
            if reserved:
                stats['FAIL'] += 1
                failures.append((child_vt, idx, child_name, reserved))
            else:
                stats['patch'] += 1           # case 6: harmless rebind

    print('slot classification across all 6 vtables (%d slots):'
          % sum(stats.values()))
    print('  case 1  locally defined child (skip)      : %d' % stats['local'])
    print('  case 3  child name == parent name (skip)   : %d' % stats['same'])
    print('  alias   resolved to the matching name      : %d' % stats['alias_ok'])
    print('  case 6  rebind to parent (harmless)        : %d' % stats['patch'])
    print('  parent entry without symbol (skip)        : %d' % stats['parent_null'])
    print('  case 4  PAD SLOT MISMATCH -> FAIL          : %d' % stats['FAIL'])
    print('  alias risk (pad-slot alias could win)      : %d' % stats['alias_risk'])
    print()
    if failures:
        print('FAILING SLOTS:')
        for child_vt, idx, child_name, reserved in failures:
            print('  %s [%3d] child=%s parent_candidates=%s'
                  % (child_vt, idx, child_name, reserved))
        print()
    print('RESULT: %s' % ('NO PAD-SLOT FAILURE' if not failures
                          else '%d SLOTS WOULD ABORT INJECTION' % len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
