import struct
path = r'C:\Windows\System32\d3d11.dll'
with open(path, 'rb') as f:
    dos = f.read(64)
    e_lfanew = struct.unpack('<I', dos[0x3C:0x40])[0]
    f.seek(e_lfanew + 4)
    fh = f.read(20)
    num_sections = struct.unpack('<H', fh[2:4])[0]
    opt_hdr_size = struct.unpack('<H', fh[16:18])[0]
    f.seek(e_lfanew + 4 + 20 + 108)
    num_rvas = struct.unpack('<I', f.read(4))[0]
    f.seek(e_lfanew + 4 + 20 + 112)
    export_rva = struct.unpack('<I', f.read(4))[0]
    export_size = struct.unpack('<I', f.read(4))[0]
    f.seek(e_lfanew + 4 + 20 + opt_hdr_size)
    sections = []
    for _ in range(num_sections):
        s = f.read(40)
        name = s[:8].rstrip(b'\x00').decode('ascii', errors='replace')
        va = struct.unpack('<I', s[12:16])[0]
        vsize = struct.unpack('<I', s[8:12])[0]
        roff = struct.unpack('<I', s[20:24])[0]
        sections.append((name, va, vsize, roff))
    def rva_to_offset(rva):
        for name, va, vsize, roff in sections:
            if va <= rva < va + vsize:
                return roff + (rva - va)
        return None
    off = rva_to_offset(export_rva)
    f.seek(off)
    edata = f.read(40)
    n_funcs = struct.unpack('<I', edata[20:24])[0]
    addr_funcs = struct.unpack('<I', edata[28:32])[0]
    print(f'Export dir: RVA=0x{export_rva:08X}, size={export_size}')
    print(f'Func table RVA=0x{addr_funcs:08X}, {n_funcs} functions')
    off_funcs = rva_to_offset(addr_funcs)
    f.seek(off_funcs)
    func_rvas = struct.unpack(f'<{n_funcs}I', f.read(n_funcs * 4))
    for i, rva in enumerate(func_rvas[:10]):
        print(f'  Func {i}: RVA=0x{rva:08X}')
    # Check for forwarders also in .rdata section
    for name, va, vsize, roff in sections:
        if name == '.rdata':
            rdata_start = va
            rdata_end = va + vsize
    for i, rva in enumerate(func_rvas):
        if export_rva <= rva < export_rva + export_size:
            off = rva_to_offset(rva)
            f.seek(off)
            fwd = b''
            while True:
                ch = f.read(1)
                if ch == b'\x00' or not ch: break
                fwd += ch
            print(f'  Ord {i}: export-range fwd -> "{fwd.decode("ascii")}"')
        elif rdata_start <= rva < rdata_end:
            off = rva_to_offset(rva)
            f.seek(off)
            fwd = b''
            while True:
                ch = f.read(1)
                if ch == b'\x00' or not ch: break
                fwd += ch
            if f'\\x00' not in str(fwd):
                print(f'  Ord {i}: rdata fwd -> "{fwd.decode("ascii", errors="replace")}"')
