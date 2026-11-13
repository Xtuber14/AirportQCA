#!/usr/bin/env python3
"""Minimal Mach-O / KC parser for vtable analysis."""
import struct

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB


class Section:
    __slots__ = ('name', 'segname', 'addr', 'size', 'offset', 'align', 'reloff', 'nreloc')

    def __init__(self, name, segname, addr, size, offset, align, relloc, nreloc):
        self.name = name
        self.segname = segname
        self.addr = addr
        self.size = size
        self.offset = offset
        self.align = align
        self.reloff = relloc
        self.nreloc = nreloc


class MachO:
    def __init__(self, data, base=0):
        self.data = data
        self.base = base
        magic = struct.unpack_from('<I', data, base)[0]
        assert magic == MH_MAGIC_64, hex(magic)
        ncmds, sizeofcmds = struct.unpack_from('<II', data, base + 16)
        p = base + 32
        end = p + sizeofcmds
        self.sections = []
        self.symtab_off = 0
        self.nsyms = 0
        self.stroff = 0
        self.dysym = None
        while p < end:
            cmd, size = struct.unpack_from('<II', data, p)
            if cmd == LC_SEGMENT_64:
                nsects = struct.unpack_from('<I', data, p + 64)[0]
                sp = p + 72
                for i in range(nsects):
                    rec = data[sp + 80 * i: sp + 80 * i + 80]
                    sname = rec[0:16].split(b'\0')[0].decode()
                    sgname = rec[16:32].split(b'\0')[0].decode()
                    addr, sz = struct.unpack_from('<QQ', rec, 32)
                    offset, align = struct.unpack_from('<II', rec, 48)
                    rel, nrel = struct.unpack_from('<II', rec, 56)
                    self.sections.append(Section(sname, sgname, addr, sz, offset, align, rel, nrel))
                sp += 80 * nsects
            elif cmd == LC_SYMTAB:
                self.symtab_off, self.nsyms, self.stroff, _ = struct.unpack_from('<IIII', data, p + 8)
            elif cmd == LC_DYSYMTAB:
                self.dysym = struct.unpack_from('<18I', data, p + 8)
            p += size
        self._syms = None

    # -- symbols -------------------------------------------------------
    def sym(self, i):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
            '<IBBHQ', self.data, self.symtab_off + 16 * i)
        end = self.data.index(b'\0', self.stroff + n_strx)
        name = self.data[self.stroff + n_strx:end].decode(errors='replace')
        return dict(strx=n_strx, type=n_type, sect=n_sect, desc=n_desc, value=n_value, name=name)

    def symbols(self):
        if self._syms is None:
            self._syms = [self.sym(i) for i in range(self.nsyms)]
        return self._syms

    def find_symbol(self, name):
        for s in self.symbols():
            if s['name'] == name:
                return s
        return None

    def symbol_names(self):
        return {s['name'] for s in self.symbols()}

    # -- sections ------------------------------------------------------
    def section_data(self, sec):
        return self.data[sec.offset: sec.offset + sec.size]

    def section_for_addr(self, addr):
        for s in self.sections:
            if s.size and s.addr <= addr < s.addr + s.size:
                return s
        return None

    def file_offset_for_addr(self, addr):
        s = self.section_for_addr(addr)
        if s is None:
            return None
        return s.offset + (addr - s.addr)

    # -- relocations ---------------------------------------------------
    def unified_relocs(self):
        """External relocations from the unified table (LC_DYSYMTAB).

        Returns dict: absolute VM address -> symbol name.
        """
        out = {}
        if not self.dysym:
            return out
        extreloff, nextrel = self.dysym[14], self.dysym[15]
        for i in range(nextrel):
            r_address, w = struct.unpack_from('<II', self.data, extreloff + 8 * i)
            r_symbolnum = w & 0xFFFFFF
            r_extern = (w >> 27) & 1
            if r_extern and r_symbolnum < len(self.symbols()):
                out[r_address] = self.symbols()[r_symbolnum]['name']
        return out

    def relocs(self, sec):
        """Return dict: r_address -> (kind, symbol_name)."""
        out = {}
        if not sec.nreloc:
            return out
        for i in range(sec.nreloc):
            r_address, w = struct.unpack_from('<II', self.data, sec.reloff + 8 * i)
            r_symbolnum = w & 0xFFFFFF
            r_pcrel = (w >> 24) & 1
            r_length = (w >> 25) & 3
            r_extern = (w >> 27) & 1
            r_type = (w >> 28) & 0xF
            if r_extern:
                if r_symbolnum < len(self.symbols()):
                    name = self.symbols()[r_symbolnum]['name']
                else:
                    name = '<ext:%d>' % r_symbolnum
                out[r_address] = ('extern', name)
            else:
                out[r_address] = ('local', (r_symbolnum, r_pcrel, r_length, r_type))
        return out

    # -- vtable --------------------------------------------------------
    def vtable_slots(self, vtable_name):
        """Decode the vtable of an object file/binary using relocations.

        Returns list of (index, symbol_name_or_None, raw_value).
        """
        sym = self.find_symbol(vtable_name)
        if sym is None:
            raise KeyError(vtable_name)
        vt_addr = sym['value']
        sec = self.section_for_addr(vt_addr)
        if sec is None:
            raise KeyError('no section for %s' % vtable_name)
        relocs = self.relocs(sec)
        base_off = vt_addr - sec.addr
        data = self.section_data(sec)
        local_by_addr = {}
        for s in self.symbols():
            if (s['type'] & 0x0E) and s['sect'] != 0:
                local_by_addr.setdefault(s['value'], s['name'])
        slots = {}
        for r_addr, (kind, val) in relocs.items():
            if r_addr < base_off:
                continue
            idx = (r_addr - base_off) // 8
            if kind == 'extern':
                slots[idx] = val
            else:
                symnum = val[0]
                name = self.symbols()[symnum]['name'] if symnum < len(self.symbols()) else None
                slots[idx] = name
        if not slots:
            return []
        maxidx = max(slots)
        out = []
        for idx in range(maxidx + 1):
            off = base_off + 8 * idx
            raw = struct.unpack_from('<Q', data, off)[0] if off + 8 <= len(data) else 0
            name = slots.get(idx)
            if name is None and raw != 0:
                name = local_by_addr.get(vt_addr + 8 * idx)
            out.append((idx, name, raw))
        return out


def load(path):
    with open(path, 'rb') as f:
        return MachO(f.read())
