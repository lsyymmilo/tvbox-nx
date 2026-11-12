import sys
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

elf_path, start_hex, length = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)

with open(elf_path, 'rb') as f:
    el = ELFFile(f)
    vaddr, size, data = None, 0, None
    for seg in el.iter_segments():
        if seg['p_type'] == 'PT_LOAD' and seg['p_vaddr'] <= start_hex < seg['p_vaddr'] + seg['p_filesz']:
            off = start_hex - seg['p_vaddr']
            data = seg.data()[off:off + length]
            vaddr = start_hex
            break
    if data is None:
        print('no segment covers', hex(start_hex))
        sys.exit(1)

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
for ins in md.disasm(data, vaddr):
    print('%#x:\t%s\t%s' % (ins.address, ins.mnemonic, ins.op_str))
