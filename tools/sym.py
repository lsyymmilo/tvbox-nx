import sys, bisect
from elftools.elf.elffile import ELFFile

elf_path = sys.argv[1]
offs = [int(x, 16) for x in sys.argv[2:]]

with open(elf_path, 'rb') as f:
    el = ELFFile(f)
    symtab = el.get_section_by_name('.symtab')
    funcs = []
    for s in symtab.iter_symbols():
        if s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
            funcs.append((s['st_value'], s['st_size'], s.name))
    funcs.sort()
    starts = [f[0] for f in funcs]

    def resolve(off):
        i = bisect.bisect_right(starts, off) - 1
        if i < 0:
            return '?'
        a, sz, name = funcs[i]
        d = off - a
        if sz and d >= sz:
            return '%s+%d (but func size %d, outside)' % (name, d, sz)
        return '%s+%#x (func %#x size %d)' % (name, d, a, sz)

    for off in offs:
        print('%#08x  ->  %s' % (off, resolve(off)))
