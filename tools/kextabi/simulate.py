#!/usr/bin/env python3
"""Faithful simulation of OpenCore's vtable patcher for a candidate kext.

Mirrors:
  Library/OcAppleKernelLib/Vtables.c
    InternalInitializeVtablePatchData
    InternalConstructVtablePrelinked
    InternalInitializeVtableByEntriesAndRelocations
    InternalPatchVtableSymbol
  Library/OcMachoLib/CxxSymbols.c
    MachoSymbolNameIsPureVirtual / MachoSymbolNameIsPadslot
  Library/OcAppleKernelLib/Link.c
    InternalOcGetSymbolValue (Cxx level)
"""
import re
import struct
import sys
import macholib
import kernel as kmod

VTABLE_HEADER_LEN = 2
PURE_VIRTUAL = '___cxa_pure_virtual'
RESERVED_TOKEN = '_RESERVED'


def demangle_class_from_vtable(name):
    """__ZTV17IO80211Controller -> IO80211Controller ; __ZTVN10AirportQCA9MetaClassE -> AirportQCA."""
    if name.startswith('__ZTVN'):
        rest = name[6:]
    elif name.startswith('__ZTV'):
        rest = name[5:]
    else:
        return None
    m = re.match(r'(\d+)', rest)
    if not m:
        return None
    n = int(m.group(1))
    return rest[len(m.group(1)):len(m.group(1)) + n]


def demangle_class_from_meta(name):
    """__ZN17IO80211Controller10gMetaClassE -> IO80211Controller"""
    m = re.match(r'__ZN(\d+)', name)
    if not m:
        return None
    n = int(m.group(1))
    return name[4 + len(m.group(1)):4 + len(m.group(1)) + n]


def class_prefix(class_name):
    return '__ZN%d%s' % (len(class_name), class_name)


class Kext:
    def __init__(self, path):
        self.m = macholib.load(path)
        self.urel = self.m.unified_relocs()
        self.local_defined = set()
        self.undefined = set()
        for s in self.m.symbols():
            if not s['name']:
                continue
            if s['type'] & 0x01 and not (s['type'] & 0x0E):
                self.undefined.add(s['name'])
            elif (s['type'] & 0x0E):
                self.local_defined.add(s['name'])
        self.local_by_value = {}
        for s in self.m.symbols():
            if (s['type'] & 0x0E) and s['sect'] and s['value'] and s['name']:
                self.local_by_value.setdefault(s['value'], s['name'])

    def vtable_slots(self, vname):
        """Return list of (raw_value, reloc_symbol_or_None) starting at raw slot 2.

        The scan stops at the first slot that is zero AND has no external
        relocation, exactly like InternalInitializeVtablePatchData.
        """
        sym = self.m.find_symbol(vname)
        if sym is None or not sym['value']:
            return None
        sec = self.m.section_for_addr(sym['value'])
        if sec is None:
            return None
        data = self.m.section_data(sec)
        base = sym['value'] - sec.addr
        slots = []
        idx = VTABLE_HEADER_LEN
        limit = (len(data) - base) // 8
        while idx < limit:
            off = base + 8 * idx
            raw = struct.unpack_from('<Q', data, off)[0]
            addr = sym['value'] + 8 * idx
            reloc = self.urel.get(addr)
            slots.append((raw, reloc))
            if raw == 0 and reloc is None:
                break
            idx += 1
        return slots

    def super_class_of(self, class_name):
        """Resolve the super class from the `superClass` pointer symbol."""
        sup = '__ZN%d%s10superClassE' % (len(class_name), class_name)
        sym = self.m.find_symbol(sup)
        if sym is None:
            return None
        tgt = self.urel.get(sym['value'])
        if tgt and 'gMetaClassE' in tgt:
            return demangle_class_from_meta(tgt)
        return None


def patch_vtable(k, child_vname, parent_entries, verbose=False):
    """Simulate InternalInitializeVtableByEntriesAndRelocations.

    parent_entries: dict {index -> name} using OpenCore's parent-entry indexing
    (index 0 == first method slot, i.e. raw slot 2).
    """
    msgs = []
    ok = True
    slots = k.vtable_slots(child_vname)
    if slots is None:
        return False, ['%s: not found' % child_vname]

    solve_symbols = [(raw, reloc) for (raw, reloc) in slots
                     if raw == 0 and reloc is not None]
    num_entries = len(slots) - 1  # excluding terminating zero slot
    parent_count = len(parent_entries)

    if num_entries < parent_count:
        msgs.append('  NOTE: child has %d entries, parent has %d (child vtable SHORTER)'
                    % (num_entries, parent_count))
        ok = False

    solve_index = 0
    child_class = demangle_class_from_vtable(child_vname)
    prefix = class_prefix(child_class) if child_class else None

    for index in range(parent_count):
        if index < len(slots) - 1:
            raw, reloc = slots[index]
        else:
            raw, reloc = 0, None
        if raw != 0:
            name = k.local_by_value.get(raw)
            if name is None:
                msgs.append('  [%3d] local value 0x%x has NO symbol (would be dropped)' % (index, raw))
                ok = False
            continue
        if solve_index >= len(solve_symbols):
            msgs.append('  [%3d] out of solve symbols (parent entry %s not represented)'
                        % (index, parent_entries.get(index)))
            break
        _r, symname = solve_symbols[solve_index]
        solve_index += 1
        parent_name = parent_entries.get(index)

        if parent_name is None:
            continue
        # 1) child symbol locally defined -> no patch
        if symname in k.local_defined:
            continue
        # 2) pure virtual
        if symname == PURE_VIRTUAL:
            continue
        # 3) same name
        if symname == parent_name:
            continue
        # 4) parent is a pad slot -> FAIL
        if RESERVED_TOKEN in parent_name:
            msgs.append('  [%3d] FAIL(padslot): child=%s parent=%s' % (index, symname, parent_name))
            ok = False
            continue
        # 5) child declares but does not implement a method of its own class
        if symname in k.undefined and prefix and symname.startswith(prefix):
            msgs.append('  [%3d] FAIL(unimplemented): child=%s' % (index, symname))
            ok = False
            continue
        # 6) patch -> fine
        if verbose:
            msgs.append('  [%3d] patch: child=%s <- parent=%s' % (index, symname, parent_name))
    return ok, msgs


def main():
    path = sys.argv[1]
    verbose = '-v' in sys.argv
    k = Kext(path)
    kern = kmod.Kernel()

    def parent_entries(vtable_name):
        """OpenCore parent-entry indexing: index 0 == raw slot 2."""
        ident, slots = kern.vtable(vtable_name)
        out = {}
        for raw_idx, name, addr in slots:
            if raw_idx < VTABLE_HEADER_LEN:
                continue
            if addr is None:
                continue  # terminator
            out[raw_idx - VTABLE_HEADER_LEN] = name
        return out

    classes = []
    for s in k.m.symbols():
        n = s['name']
        if n and n.endswith('10superClassE'):
            m = re.match(r'__ZN(\d+)', n)
            if not m:
                continue
            ln = int(m.group(1))
            classes.append(n[4 + len(m.group(1)):4 + len(m.group(1)) + ln])

    all_ok = True
    for cls in classes:
        sup = k.super_class_of(cls)
        print('=== class %s (super: %s)' % (cls, sup))
        targets = []
        if sup:
            targets.append(('class', '__ZTV%d%s' % (len(cls), cls),
                            '__ZTV%d%s' % (len(sup), sup)))
        targets.append(('meta', '__ZTVN%d%s9MetaClassE' % (len(cls), cls),
                        '__ZTV11OSMetaClass'))
        for kind, vname, parent in targets:
            pentries = parent_entries(parent)
            if not pentries:
                print('  %-5s %-45s : NO KERNEL DUMP (%s)' % (kind, vname, parent))
                continue
            ok, msgs = patch_vtable(k, vname, pentries, verbose)
            print('  %-5s %-45s vs %-38s : %s' % (kind, vname, parent, 'OK' if ok else 'FAIL'))
            for msg in msgs:
                print(msg)
            if not ok:
                all_ok = False

    print()
    print('RESULT: %s' % ('ALL VTABLES PATCH OK' if all_ok else 'FAILURES DETECTED'))
    return 0 if all_ok else 1


if __name__ == '__main__':
    sys.exit(main())
