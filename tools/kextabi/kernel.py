#!/usr/bin/env python3
"""Kernel ground truth with OpenCore's exact symbol-table semantics.

Reproduces:
  Library/OcAppleKernelLib/PrelinkedKext.c :: InternalScanBuildLinkedSymbolTable
  Library/OcAppleKernelLib/Link.c :: InternalOcGetSymbolWorkerValue (Cxx level)
  Library/OcAppleKernelLib/KernelCollection.c :: KcFixupValue
so that vtable entry names are exactly what OpenCore would see.
"""
import struct
import kclib

KC = '/System/Library/KernelCollections/BootKernelExtensions.kc'
KERNEL_ADDRESS_MASK = 0xFFFFFFFF00000000
KERNEL_ADDRESS_BASE = 0xFFFFFF8000000000
KERNEL_ADDRESS_KEXT = 0xFFFFFF7F00000000
KERNEL_FIXUP_OFFSET = 0x100000


def kc_fixup(value):
    if ((value & KERNEL_ADDRESS_MASK) != KERNEL_ADDRESS_BASE
            and (value & KERNEL_ADDRESS_MASK) != KERNEL_ADDRESS_KEXT):
        target = value & 0x3FFFFFFF
        cache = (value >> 30) & 3
        diversity = (value >> 32) & 0xFFFF
        if cache or diversity:
            return None
        value = target + KERNEL_FIXUP_OFFSET + KERNEL_ADDRESS_BASE
    return value


class Kernel:
    def __init__(self, path=KC):
        self.data, self.top, self.entries = kclib.parse_fileset(path)
        self._tables = {}
        self._vtables = {}
        self._global = None

    def global_symbol_map(self):
        """address -> set of C++ symbol names across every KC entry.

        OpenCore resolves a vtable entry symbol through the owning kext's own
        symbol table first and then through its dependency tree, so this map
        is the superset of every name OpenCore could ever come up with.
        """
        if self._global is None:
            gmap = {}
            for ident, off, vm in self.entries:
                try:
                    m = kclib.entry_macho(self.data, off)
                except Exception:
                    continue
                if not m.nsyms:
                    continue
                for s in m.symbols():
                    if not s['name'] or not s['name'].startswith('__Z'):
                        continue
                    if not (s['type'] & 0x0E) or not s['value']:
                        continue
                    gmap.setdefault(s['value'], set()).add(s['name'])
            self._global = gmap
        return self._global

    def _entry_macho(self, ident):
        d = {i: (o, v) for i, o, v in self.entries}
        off, _vm = d[ident]
        return kclib.entry_macho(self.data, off)

    def linked_symbol_table(self, ident):
        """Mirror of InternalScanBuildLinkedSymbolTable."""
        if ident in self._tables:
            return self._tables[ident]
        m = self._entry_macho(ident)
        syms = m.symbols()
        n = len(syms)
        table = [None] * n
        bottom = 0
        top = n - 1
        num_cxx = 0
        num_disc = 0
        for s in syms:
            t = s['type']
            if t & 0xE0:            # STAB
                num_disc += 1
                continue
            if (t & 0x0E) == 0:     # UNDF
                num_disc += 1
                continue
            if s['name'].startswith('__Z'):
                table[top] = (s['value'], s['name'])
                top -= 1
                num_cxx += 1
            else:
                table[bottom] = (s['value'], s['name'])
                bottom += 1
        if num_disc > 0:
            dst = n - num_cxx - num_disc
            src = n - num_cxx
            table[dst:dst + num_cxx] = table[src:src + num_cxx]
            n -= num_disc
        self._tables[ident] = (table, n, num_cxx)
        return self._tables[ident]

    def lookup_value(self, ident, value):
        """Mirror of InternalOcGetSymbolWorkerValue with OcGetSymbolOnlyCxx.

        The owning kext's own table is searched first (with OpenCore's exact
        ordering, so the *last* C++ symbol in symtab order wins).  When that
        fails OpenCore continues through the dependency tree, which is modelled
        here by the global map.
        """
        table, n, num_cxx = self.linked_symbol_table(ident)
        start = (n - num_cxx) & ~15
        for i in range(start, min(start + num_cxx, len(table))):
            e = table[i]
            if e is not None and e[0] == value:
                return e[1]
        # Dependency-tree fallback.
        cands = self.global_symbol_map().get(value)
        if cands:
            # Deterministic pick: the last name in sorted order, matching the
            # "last symbol wins" spirit of OpenCore's table scan.
            return sorted(cands)[-1]
        return None

    def lookup_candidates(self, value):
        """Every C++ name OpenCore could resolve for `value`."""
        return self.global_symbol_map().get(value, set())

    def resolve_per_entry(self, value):
        """ident -> the name OpenCore's own-entry lookup yields for `value`.

        Used to disambiguate aliases (two symbols at one address): OpenCore
        resolves an address through exactly one kext's own table, and that
        table's ordering decides the winner.
        """
        out = {}
        for ident, off, vm in self.entries:
            try:
                m = kclib.entry_macho(self.data, off)
            except Exception:
                continue
            if not m.nsyms:
                continue
            table, n, num_cxx = self.linked_symbol_table(ident)
            start = (n - num_cxx) & ~15
            for i in range(start, min(start + num_cxx, len(table))):
                e = table[i]
                if e is not None and e[0] == value:
                    out[ident] = e[1]
                    break
        return out

    def vtable(self, vtable_name):
        """Return (ident, [(index, name_or_None, address_or_None), ...])."""
        if vtable_name in self._vtables:
            return self._vtables[vtable_name]
        for ident, off, vm in self.entries:
            try:
                m = kclib.entry_macho(self.data, off)
            except Exception:
                continue
            sym = m.find_symbol(vtable_name)
            if not sym or not sym['value']:
                continue
            sec = None
            for sg in m.segs:
                for sc in sg.sects:
                    if sc.size and sc.addr <= sym['value'] < sc.addr + sc.size:
                        sec = sc
            if sec is None:
                continue
            fo = sec.offset + (sym['value'] - sec.addr)
            nexts = [s['value'] for s in m.symbols()
                     if s['value'] > sym['value'] and s['value'] < sec.addr + sec.size]
            end = min(nexts) if nexts else sec.addr + sec.size
            total = (end - sym['value']) // 8
            out = []
            for idx in range(total):
                raw = struct.unpack_from('<Q', self.data, fo + 8 * idx)[0]
                if raw == 0:
                    out.append((idx, None, None))
                    continue
                addr = kc_fixup(raw)
                name = self.lookup_value(ident, addr) if addr is not None else None
                out.append((idx, name, addr))
            self._vtables[vtable_name] = (ident, out)
            return self._vtables[vtable_name]
        self._vtables[vtable_name] = (None, [])
        return self._vtables[vtable_name]


if __name__ == '__main__':
    import sys
    k = Kernel()
    ident, slots = k.vtable(sys.argv[1] if len(sys.argv) > 1 else '__ZTV11OSMetaClass')
    print('# in %s' % ident)
    for idx, name, addr in slots:
        print('[%3d] %s' % (idx, name if name else ('0' if addr is None else '?@%s' % hex(addr))))
