#!/usr/bin/env python3
"""Kernel collection (MH_FILESET) parser."""
import struct

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_FILESET_ENTRY = 0x80000035
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_UUID = 0x1B


class Seg:
    __slots__ = ('name', 'vmaddr', 'vmsize', 'fileoff', 'filesize', 'nsects', 'sects')

    def __init__(self, name, vmaddr, vmsize, fileoff, filesize):
        self.name = name
        self.vmaddr = vmaddr
        self.vmsize = vmsize
        self.fileoff = fileoff
        self.filesize = filesize
        self.nsects = 0
        self.sects = []


class Sec:
    __slots__ = ('name', 'segname', 'addr', 'size', 'offset', 'flags')

    def __init__(self, name, segname, addr, size, offset, flags):
        self.name = name
        self.segname = segname
        self.addr = addr
        self.size = size
        self.offset = offset
        self.flags = flags


class MachO:
    def __init__(self, data, base=0):
        self.data = data
        self.base = base
        magic = struct.unpack_from('<I', data, base)[0]
        assert magic == MH_MAGIC_64, hex(magic)
        ncmds, sizeofcmds = struct.unpack_from('<II', data, base + 16)
        p = base + 32
        end = p + sizeofcmds
        self.segs = []
        self.symtab_off = 0
        self.nsyms = 0
        self.stroff = 0
        self.chained_fixups = None
        self.fileset_entries = []
        while p < end:
            cmd, size = struct.unpack_from('<II', data, p)
            if cmd == LC_SEGMENT_64:
                segname = data[p + 8:p + 24].split(b'\0')[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, p + 24)
                nsects = struct.unpack_from('<I', data, p + 64)[0]
                seg = Seg(segname, vmaddr, vmsize, fileoff, filesize)
                seg.nsects = nsects
                sp = p + 72
                for i in range(nsects):
                    rec = data[sp + 80 * i: sp + 80 * i + 80]
                    sname = rec[0:16].split(b'\0')[0].decode()
                    sgname = rec[16:32].split(b'\0')[0].decode()
                    addr, sz = struct.unpack_from('<QQ', rec, 32)
                    offset = struct.unpack_from('<I', rec, 48)[0]
                    flags = struct.unpack_from('<I', rec, 64)[0]
                    seg.sects.append(Sec(sname, sgname, addr, sz, offset, flags))
                self.segs.append(seg)
            elif cmd == LC_SYMTAB:
                self.symtab_off, self.nsyms, self.stroff, _ = struct.unpack_from('<IIII', data, p + 8)
            elif cmd == LC_FILESET_ENTRY:
                vmaddr = struct.unpack_from('<Q', data, p + 8)[0]
                fileoff = struct.unpack_from('<Q', data, p + 16)[0]
                ident = data[p + 32:data.index(b'\0', p + 32)].decode(errors='replace')
                self.fileset_entries.append((vmaddr, fileoff, ident))
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                foff, fsize = struct.unpack_from('<II', data, p + 8)
                self.chained_fixups = (foff, fsize)
            p += size
        self._syms = None

    def sym(self, i):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
            '<IBBHQ', self.data, self.symtab_off + 16 * i)
        end = self.data.index(b'\0', self.stroff + n_strx)
        name = self.data[self.stroff + n_strx:end].decode(errors='replace')
        return dict(type=n_type, sect=n_sect, desc=n_desc, value=n_value, name=name)

    def symbols(self):
        if self._syms is None:
            self._syms = [self.sym(i) for i in range(self.nsyms)]
        return self._syms

    def find_symbol(self, name):
        for s in self.symbols():
            if s['name'] == name:
                return s
        return None

    def section_for_addr(self, addr):
        for seg in self.segs:
            for s in seg.sects:
                if s.size and s.addr <= addr < s.addr + s.size:
                    return s
        return None


def parse_fileset(path):
    with open(path, 'rb') as f:
        data = f.read()
    top = MachO(data, 0)
    entries = []
    for vmaddr, off, ident in top.fileset_entries:
        entries.append((ident, off, vmaddr))
    return data, top, entries


def entry_macho(data, off):
    return MachO(data, off)
