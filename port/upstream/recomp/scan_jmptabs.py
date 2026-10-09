import re
import struct
import sys
import image
from x86dec import decode

sp, _, _ = image.load()
known = {}
cur = None
for line in open('../src/gen/dispatch.c'):
    m = re.match(r'const FnEntry tab_(\w+)\[\]', line)
    if m:
        cur = m.group(1); known[cur] = set(); continue
    m = re.match(r'\s*\{0x([0-9a-f]+), 0x([0-9a-f]+), f_', line)
    if m and cur:
        known[cur].add(((int(m.group(1), 16) - image.LOADSEG) & 0xFFFF, int(m.group(2), 16)))

out = set()
for name, s in sp.items():
    if name not in known or name == 'fm':
        continue
    segs = sorted({g for g, _ in known[name]})
    for seg in segs:
        base = s.pos(seg, 0)
        lo_off = max(0, -base); hi_off = min(0x10000, s.size - base)
        o = lo_off
        while o < hi_off - 2:
            i = decode(s.data, base + o, base) if base + o >= 0 else None
            if i is None:
                o += 1; continue
            if i.op == 'jmpi' and i.args and i.args[0][0] == 'r16':
                for t in range(o + i.length, min(o + 0x100, hi_off - 6)):
                    run = []
                    k = t
                    while k + 2 <= hi_off:
                        v = struct.unpack_from('<H', s.data, base + k)[0]
                        if abs(v - o) > 0x800 or not (lo_off <= v < hi_off):
                            break
                        if decode(s.data, base + v, base) is None:
                            break
                        run.append(v); k += 2
                    if len(run) >= 3:
                        for v in run:
                            if (seg, v) not in known[name]:
                                out.add('%s %04x:%04x' % (name, seg, v))
                        break
            o += i.length
print('\n'.join(sorted(out)))
print('%d jump-table targets' % len(out), file=sys.stderr)
