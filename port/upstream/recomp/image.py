import hashlib
import os
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from versions import V, NAME as VERSION

GAME_DIR = os.environ.get('POP2_GAMEDIR', 'D:/Dos/prince 2.pc')
EXE_PATH = os.path.join(GAME_DIR, 'prince.exe')
LOADSEG = 0x0100
OVL_DESC = V.OVL_DESC
N_OVL = V.N_OVL
OVL_COMPRESSED = 0x20
DUMP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ovl', VERSION)


class Space:
    def __init__(self, name, linkseg, data, fileoff, reloc_sites):
        self.name = name
        self.linkseg = linkseg
        self.data = data
        self.fileoff = fileoff
        self.reloc = reloc_sites
        self.size = len(data)
        self.filesize = len(data)
        self.memsize = len(data)
        self.flags = 0

    def contains(self, seg, off):
        lin = (seg - self.linkseg) * 16 + off
        return 0 <= lin < self.size

    def pos(self, seg, off):
        return (seg - self.linkseg) * 16 + off


def load():
    x = bytearray(open(EXE_PATH, 'rb').read())
    sha = hashlib.sha256(x).hexdigest()
    if getattr(V, 'EXE_SHA256', sha) != sha:
        raise SystemExit('%s: sha256 %s, not the %s exe %s' % (EXE_PATH, sha, VERSION, V.EXE_SHA256))
    h = struct.unpack_from('<14H', x, 0)
    cblp, cp, crlc, hdr = h[1], h[2], h[3], h[4]
    lfarlc = h[12]
    img0 = hdr * 16
    img1 = cp * 512 - (512 - cblp if cblp else 0)
    for off, (old, new) in getattr(V, 'PATCHES', {}).items():
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        if x[img0 + off:img0 + off + len(old)] != old:
            raise SystemExit('%s: root 0x%x holds %s, not %s' % (EXE_PATH, off, x[img0 + off:img0 + off + len(old)].hex(), old.hex()))
        x[img0 + off:img0 + off + len(new)] = new
    root = bytearray(x[img0:img1])
    sites = set()
    for i in range(crlc):
        off, seg = struct.unpack_from('<HH', x, lfarlc + 4 * i)
        p = seg * 16 + off
        v = struct.unpack_from('<H', root, p)[0]
        struct.pack_into('<H', root, p, (v + LOADSEG) & 0xFFFF)
        sites.add(p)
    spaces = {'root': Space('root', 0, root, img0, sites)}
    header = dict(ss=h[7], sp=h[8], ip=h[10], cs=h[11], minalloc=h[5], maxalloc=h[6])
    for k in range(N_OVL):
        o = OVL_DESC + 18 * k
        seg, f2, pos, hi, fl, f8, nrel, fc, fe, size = struct.unpack_from('<HHHBBHHHHH', x, o)
        rpos = (pos + hi * 65536) * 16
        code = rpos + ((nrel + 3) >> 2) * 16
        name = 'o%d' % fe
        sites = set()
        if fl & OVL_COMPRESSED:
            dump = os.path.join(DUMP_DIR, name + '.bin')
            data = bytearray(open(dump, 'rb').read()) if os.path.exists(dump) else bytearray()
            if data and len(data) != f8 * 16:
                raise SystemExit('%s: %d bytes, expected %d (f8 %d * 16)' % (dump, len(data), f8 * 16, f8))
            for i in range(nrel if data else 0):
                roff, rseg = struct.unpack_from('<HH', x, rpos + 4 * i)
                p = (rseg - seg) * 16 + roff
                if 0 <= p < f8 * 16 - 1:
                    sites.add(p)
            memsize = f8 * 16
        else:
            data = bytearray(x[code:code + size * 16])
            for i in range(nrel):
                roff, rseg = struct.unpack_from('<HH', x, rpos + 4 * i)
                p = (rseg - seg) * 16 + roff
                if 0 <= p < len(data) - 1:
                    v = struct.unpack_from('<H', data, p)[0]
                    struct.pack_into('<H', data, p, (v + LOADSEG) & 0xFFFF)
                    sites.add(p)
            memsize = size * 16
        spaces[name] = Space(name, seg, data, code, sites)
        spaces[name].flags = fl
        spaces[name].ovl_id = fe
        spaces[name].filesize = size * 16
        spaces[name].memsize = memsize
    comp = [s for s in spaces.values() if s.flags & OVL_COMPRESSED and s.size]
    for i, a in enumerate(comp):
        for b in comp[i + 1:]:
            n = min(a.size, b.size)
            if a.linkseg == b.linkseg and a.data[:n] == b.data[:n]:
                raise SystemExit('%s/%s.bin and %s.bin hold the same code: one was dumped while the other was resident; delete the smaller/later one'
                                 % (DUMP_DIR, a.name, b.name))
    return spaces, header, x


if __name__ == '__main__':
    sp, hdr, _ = load()
    print(hdr)
    for n, s in sp.items():
        print(n, hex(s.linkseg), s.size, 'file', s.fileoff, s.filesize, 'mem', s.memsize, 'flags', s.flags,
              'relocs', len(s.reloc), '' if s.size or not s.flags & OVL_COMPRESSED else '(no dump yet)')
