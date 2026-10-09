import os, sys, struct, collections


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, 'w').write(text)
from x86dec import decode, R8, R16, SREG, JCC
from image import load, LOADSEG, GAME_DIR, OVL_COMPRESSED, VERSION, DUMP_DIR
from versions import V

ROOT_END = V.ROOT_END
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'gen')

spaces, HDR, EXE = load()

HOOKS = V.HOOKS

FM_SEG = 0xD000
FM_DRV = os.path.join(GAME_DIR, 'msb_pro.drv')
FM_ENTRIES = set()
if os.path.exists(FM_DRV):
    from image import Space
    _d = bytearray(open(FM_DRV, 'rb').read())
    _fm = Space('fm', FM_SEG - LOADSEG, _d, 0x7FFF0000, set())
    _fm.ovl_id = 100
    _fm.flags = 1
    spaces['fm'] = _fm
    FM_ENTRIES = {0x100, 0x3d2} | {struct.unpack_from('<H', _d, 0x123 + 2 * i)[0] for i in range(16)}


def spaces_for(linkseg, off):
    if linkseg < ROOT_END:
        s = spaces['root']
        return [s] if s.contains(linkseg, off) else []
    return [s for n, s in spaces.items() if n != 'root' and getattr(s, 'flags', 0) != 1
            and s.contains(linkseg, off)]


BLOCK_END = ('jmp', 'jmpf', 'jmpi', 'jmpfi', 'ret', 'retf', 'iret', 'hlt', 'loop', 'loope', 'loopne', 'jcxz')


def far_clean(space, seg, off):
    a = dec_at(space, seg, off)
    b = dec_at(space, seg, (off + a.length) & 0xFFFF) if a else None
    if a and b and a.op == 'push' and a.args[0] == ('r16', 5) and b.op == 'mov' and b.args[0] == ('r16', 5) \
            and b.args[1] == ('r16', 4):
        return True
    o = off
    for _ in range(64):
        ins = dec_at(space, seg, o)
        if ins is None:
            return False
        if (ins.op in ('lea', 'les', 'lds') and ins.args[1][0] != 'mem') or \
                (ins.op in ('callfi', 'jmpfi') and ins.args[0][0] != 'mem'):
            return False
        if ins.op in BLOCK_END or ins.op in JCC:
            return True
        o = (o + ins.length) & 0xFFFF
    return True


class Func:
    def __init__(self, space, seg, off):
        self.space = space; self.seg = seg; self.off = off
        self.insns = {}
        self.labels = set()
        self.calls = []
        self.bad = False
        self.tables = {}
        self.table_seg = {}

    @property
    def name(self):
        return 'f_%s_%04x_%04x' % (self.space.name, self.seg, self.off)


def dec_at(space, seg, off):
    p = space.pos(seg, off)
    if p < 0 or p >= space.size - 1:
        return None
    base = p - off
    buf = space.data
    try:
        ins = decode(buf, p, base)
    except IndexError:
        return None
    return ins


def table_bound(fn, ins):
    n = None
    between = []
    for o in sorted(fn.insns, reverse=True):
        if o >= ins.addr:
            continue
        if ins.addr - o > 40:
            break
        i = fn.insns[o]
        if i.op == 'cmp' and i.args[1][0] == 'imm' and i.args[0][0] in ('r16', 'r8'):
            n = i.args[1][1] + 1
            if byte_index(i, between):
                n = i.args[1][1] // 2 + 1
            break
        between.append(i)
    return n


def byte_index(cmp, between):
    idx = {'bx'}
    cbw = False
    for i in between:
        a = i.args
        if i.op in JCC or (i.op == 'jmp' and a and a[0][0] == 'rel'):
            continue
        if i.op == 'mov' and a[0][0] == 'r16' and a[1][0] == 'r16':
            d, s = R16[a[0][1]], R16[a[1][1]]
            if d in idx:
                idx = (idx - {d}) | {s}
            continue
        if i.op == 'xchg' and a[0][0] == 'r16' and a[1][0] == 'r16':
            x, y = R16[a[0][1]], R16[a[1][1]]
            idx = (idx - {x, y}) | ({y} if x in idx else set()) | ({x} if y in idx else set())
            continue
        if i.op == 'cbw' and 'ax' in idx:
            cbw = True
            continue
        return False
    r, imm = cmp.args[0], cmp.args[1][1]
    if r[0] == 'r16':
        return R16[r[1]] in idx and not cbw
    return R8[r[1]] == 'al' and cbw and 'ax' in idx and imm < 0x80


def code_stops(fn):
    sp = fn.space
    stops = {lin for (name, lin) in ENTRIES_BY_LIN if name == sp.name}
    base = sp.pos(fn.seg, 0)
    stops.update(base + o for o in fn.insns)
    return stops


def find_table(fn, ins, n, stops=frozenset()):
    a = ins.args[0]
    if a[0] != 'mem' or a[5] != 'cs' or a[3] != 'bx':
        return None
    table = a[4] & 0xFFFF
    sp = fn.space
    count = n if n is not None and n <= 256 else 64

    def read(S):
        targets = []
        for k in range(count):
            p = sp.pos(S, table + 2 * k)
            if p < 0 or p + 2 > sp.size:
                break
            if p in stops or p + 1 in stops:
                return targets, True
            w = struct.unpack_from('<H', sp.data, p)[0]
            t = (w + (S - fn.seg) * 16) & 0xFFFF
            if n is None and (abs(t - ins.addr) > 0x2000):
                break
            targets.append((w, t))
        return targets, False

    def sound(r, cut_ok=False):
        tb, cut = r
        return bool(tb) and (n is None or len(tb) == n or (cut_ok and cut)) and all(abs(t - ins.addr) <= 0x2000 and dec_at(sp, fn.seg, t) is not None for w, t in tb)
    targets = read(fn.seg)
    if not sound(targets):
        others = [S for S in sorted(space_segs(sp)) if S != fn.seg]
        for S in others:
            cand = read(S)
            if sound(cand):
                fn.table_seg[ins.addr] = S
                return cand[0]
        if sound(targets, True):
            return targets[0]
        for S in others:
            cand = read(S)
            if sound(cand, True):
                fn.table_seg[ins.addr] = S
                return cand[0]
    return targets[0]


def space_segs(space):
    segs = {s for (name, lin), (s, o) in ENTRIES_BY_LIN.items() if name == space.name}
    segs.add(space.linkseg)
    return segs


ENTRIES_BY_LIN = {}


def near_target(space, seg, t):
    if space.contains(seg, t):
        return seg, t
    for adj in (-0x10000, 0x10000):
        lin = space.pos(seg, t) + adj
        if 0 <= lin < space.size:
            k = (space.name, lin)
            if k in ENTRIES_BY_LIN:
                return ENTRIES_BY_LIN[k]
            s2 = space.linkseg + (max(0, lin - 0xFFFF) + 15) // 16
            return s2, lin - (s2 - space.linkseg) * 16
    return seg, t


def trace(fn, entries_global):
    ENTRIES_BY_LIN.setdefault((fn.space.name, fn.space.pos(fn.seg, fn.off)), (fn.seg, fn.off))
    work = [fn.off]
    seen = set()
    pending = []
    while work or pending:
        if not work:
            o, n = pending.pop(0)
            tb = find_table(fn, fn.insns[o], n, code_stops(fn))
            if tb:
                tb = [(w, t) for w, t in tb if dec_at(fn.space, fn.seg, t) is not None]
                fn.tables[o] = tb
                for w, t in tb:
                    fn.labels.add(t); work.append(t)
            continue
        o = work.pop()
        while True:
            if o in seen:
                break
            if o != fn.off and o in entries_global and o in fn.insns:
                break
            seen.add(o)
            ins = dec_at(fn.space, fn.seg, o)
            if ins is None:
                fn.bad = True
                break
            fn.insns[o] = ins
            op = ins.op
            nxt = (o + ins.length) & 0xFFFF
            if op in ('jmp',) and ins.args[0][0] == 'rel':
                t = ins.args[0][1]
                s2, t2 = near_target(fn.space, fn.seg, t)
                if (s2, t2) != (fn.seg, t):
                    fn.calls.append(('tail', s2, t2))
                    break
                if t != fn.off and t in entries_global:
                    fn.calls.append(('tail', fn.seg, t))
                    break
                fn.labels.add(t); o = t
                continue
            if op in JCC or op in ('loop', 'loope', 'loopne', 'jcxz'):
                t = ins.args[0][1]
                fn.labels.add(t); work.append(t)
                o = nxt
                continue
            if op == 'call':
                t = ins.args[0][1]
                fn.calls.append(('near',) + near_target(fn.space, fn.seg, t))
                o = nxt
                continue
            if op == 'callf':
                s, t = ins.args[0][1], ins.args[0][2]
                fn.calls.append(('far', (s - LOADSEG) & 0xFFFF, t))
                o = nxt
                continue
            if op == 'jmpf':
                s, t = ins.args[0][1], ins.args[0][2]
                fn.calls.append(('far', (s - LOADSEG) & 0xFFFF, t))
                break
            if op == 'jmpi':
                pending.append((o, table_bound(fn, ins)))
                break
            if op in ('ret', 'retf', 'iret', 'jmpfi', 'hlt'):
                break
            o = nxt
    return fn


REG16 = {'ax': 'AX', 'cx': 'CX', 'dx': 'DX', 'bx': 'BX', 'sp': 'SP', 'bp': 'BP', 'si': 'SI', 'di': 'DI'}
REG8 = {'al': 'AL', 'cl': 'CL', 'dl': 'DL', 'bl': 'BL', 'ah': 'AH', 'ch': 'CH', 'dh': 'DH', 'bh': 'BH'}
SEGR = {'es': 'ES', 'cs': 'CS', 'ss': 'SS', 'ds': 'DS'}


class Gen:
    def __init__(self, fn):
        self.fn = fn
        self.cs = (fn.seg + LOADSEG) & 0xFFFF
        self.out = []

    def segval(self, s):
        return '0x%04x' % self.cs if s == 'cs' else SEGR[s]

    def ea(self, a):
        if len(a) != 6:
            raise NotImplementedError('register operand %r in a memory-only form' % (a,))
        _, size, sdef, base, disp, sov = a
        seg = self.segval(sov or sdef)
        terms = [REG16[b] for b in base.split('+')] if base else []
        off = ' + '.join(terms + ['%d' % disp]) if terms else '0x%04x' % (disp & 0xFFFF)
        return seg, '(uint16_t)(%s)' % off

    def rd(self, a, size=None):
        k = a[0]
        if k == 'r8': return REG8[R8[a[1]]]
        if k == 'r16': return REG16[R16[a[1]]]
        if k == 'sreg': return SEGR[SREG[a[1]]] if SREG[a[1]] != 'cs' else '0x%04x' % self.cs
        if k == 'imm': return '0x%x' % a[1]
        if k == 'mem':
            s, o = self.ea(a)
            return ('RB(%s, %s)' if a[1] == 8 else 'RW(%s, %s)') % (s, o)
        raise ValueError(a)

    def wr(self, a, val):
        k = a[0]
        if k == 'r8': return '%s = (uint8_t)(%s);' % (REG8[R8[a[1]]], val)
        if k == 'r16': return '%s = (uint16_t)(%s);' % (REG16[R16[a[1]]], val)
        if k == 'sreg':
            if SREG[a[1]] == 'cs':
                return '/* mov cs */;'
            return '%s = (uint16_t)(%s);' % (SEGR[SREG[a[1]]], val)
        if k == 'mem':
            s, o = self.ea(a)
            return ('WB(%s, %s, %s);' if a[1] == 8 else 'WW(%s, %s, %s);') % (s, o, val)
        raise ValueError(a)

    def emit(self, s):
        self.out.append('    ' + s)

    def goto(self, t):
        if t in self.fn.insns:
            if t <= self.cur:
                return '{ POLL(); goto L_%04x; }' % t
            return 'goto L_%04x;' % t
        return '{ rt_badjump(0x%04x, 0x%04x, 0x%04x); return; }' % (self.cs, self.cur, t)

    def gen_ins(self, ins):
        op = ins.op; a = ins.args; sz = ins.size; S = '8' if sz == 8 else '16'
        nxt = (ins.addr + ins.length) & 0xFFFF
        E = self.emit
        if op == 'mov':
            E(self.wr(a[0], self.rd(a[1]))); return
        if op in ('add', 'adc', 'sub', 'sbb', 'and', 'or', 'xor'):
            E(self.wr(a[0], 'op_%s%s(%s, %s)' % (op, S, self.rd(a[0]), self.rd(a[1])))); return
        if op == 'cmp':
            E('op_sub%s(%s, %s);' % (S, self.rd(a[0]), self.rd(a[1]))); return
        if op == 'test':
            E('op_and%s(%s, %s);' % (S, self.rd(a[0]), self.rd(a[1]))); return
        if op in ('inc', 'dec', 'neg', 'not'):
            E(self.wr(a[0], 'op_%s%s(%s)' % (op, 'S' if False else S, self.rd(a[0])) if op != 'not'
                      else '~(%s)' % self.rd(a[0]))); return
        if op in ('rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sal', 'sar'):
            o2 = 'shl' if op == 'sal' else op
            E(self.wr(a[0], 'op_%s%s(%s, %s)' % (o2, S, self.rd(a[0]), self.rd(a[1])))); return
        if op == 'xchg':
            E('{ uint%s_t t_ = %s; %s %s }' % (S, self.rd(a[0]), self.wr(a[0], self.rd(a[1])), self.wr(a[1], 't_'))); return
        if op == 'lea':
            s, o = self.ea(a[1]); E(self.wr(a[0], o)); return
        if op in ('les', 'lds'):
            s, o = self.ea(a[1])
            E('{ uint16_t o_ = %s; %s %s = RW(%s, (uint16_t)(o_ + 2)); }' % (o, self.wr(a[0], 'RW(%s, o_)' % s),
                                                                         'ES' if op == 'les' else 'DS', s)); return
        if op == 'push':
            E('PUSH(%s);' % self.rd(a[0])); return
        if op == 'pop':
            if a[0][0] == 'mem':
                E('{ uint16_t v_ = POP(); %s }' % self.wr(a[0], 'v_'))
            else:
                E(self.wr(a[0], 'POP()'))
            return
        if op == 'pushf': E('PUSH(get_flags());'); return
        if op == 'popf': E('set_flags(POP());'); return
        if op == 'pusha': E('{ uint16_t s_ = SP; PUSH(AX); PUSH(CX); PUSH(DX); PUSH(BX); PUSH(s_); PUSH(BP); PUSH(SI); PUSH(DI); }'); return
        if op == 'popa': E('{ DI = POP(); SI = POP(); BP = POP(); POP(); BX = POP(); DX = POP(); CX = POP(); AX = POP(); }'); return
        if op == 'cbw': E('AX = (uint16_t)(int16_t)(int8_t)AL;'); return
        if op == 'cwd': E('DX = (AX & 0x8000) ? 0xFFFF : 0;'); return
        if op == 'lahf': E('AH = (uint8_t)get_flags();'); return
        if op == 'sahf': E('set_flags((get_flags() & 0xFF00) | AH);'); return
        if op == 'clc': E('FC = 0;'); return
        if op == 'stc': E('FC = 1;'); return
        if op == 'cmc': E('FC ^= 1;'); return
        if op == 'cld': E('FD = 0;'); return
        if op == 'std': E('FD = 1;'); return
        if op == 'cli': E('FI = 0;'); return
        if op == 'sti': E('FI = 1;'); return
        if op in ('nop', 'wait'): return
        if op == 'mul':
            E('op_mul%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'imul':
            E('op_imul%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'div':
            E('op_div%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'idiv':
            E('op_idiv%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'imul3':
            E(self.wr(a[0], 'op_imul3(%s, %s)' % (self.rd(a[1]), self.rd(a[2])))); return
        if op in ('daa', 'das', 'aaa', 'aas', 'aam', 'aad'):
            E('op_%s();' % op); return
        if op == 'xlat':
            E('AL = RB(%s, (uint16_t)(BX + AL));' % self.segval(ins.seg or 'ds')); return
        if op in ('movsb', 'movsw', 'stosb', 'stosw', 'lodsb', 'lodsw', 'cmpsb', 'cmpsw', 'scasb', 'scasw'):
            seg = self.segval(ins.seg or 'ds')
            E('str_%s(%s, %d);' % (op, seg, {'': 0, 'rep': 1, 'repne': 2}[ins.rep])); return
        if op == 'in':
            port = self.rd(a[1])
            E('%s = port_in%s(%s);' % (self.rd(a[0]), S, port)); return
        if op == 'out':
            E('port_out%s(%s, %s);' % (S, self.rd(a[0]), self.rd(a[1]))); return
        if op == 'int':
            E('do_int(0x%x, 0x%04x, 0x%04x);' % (a[0][1], self.cs, nxt)); return
        if op == 'into':
            E('if (FO) do_int(4, 0x%04x, 0x%04x);' % (self.cs, nxt)); return
        if op == 'enter':
            E('{ PUSH(BP); uint16_t f_ = SP; BP = f_; SP -= 0x%x; }' % a[0][1]); return
        if op == 'leave':
            E('SP = BP; BP = POP();'); return
        if op == 'bound':
            return
        if op == 'esc':
            E('fpu_esc(0x%x);' % a[0][1]); return
        if op == 'hlt':
            E('rt_halt(0x%04x, 0x%04x); return;' % (self.cs, ins.addr)); return
        if op in JCC:
            cond = {'jo': 'FO', 'jno': '!FO', 'jb': 'FC', 'jae': '!FC', 'je': 'FZ', 'jne': '!FZ',
                    'jbe': '(FC || FZ)', 'ja': '(!FC && !FZ)', 'js': 'FS', 'jns': '!FS', 'jp': 'FP',
                    'jnp': '!FP', 'jl': '(FS != FO)', 'jge': '(FS == FO)', 'jle': '(FZ || FS != FO)',
                    'jg': '(!FZ && FS == FO)'}[op]
            E('if (%s) %s' % (cond, self.goto(a[0][1]))); return
        if op == 'jcxz':
            E('if (CX == 0) %s' % self.goto(a[0][1])); return
        if op == 'loop':
            E('if (--CX != 0) %s' % self.goto(a[0][1])); return
        if op == 'loope':
            E('if (--CX != 0 && FZ) %s' % self.goto(a[0][1])); return
        if op == 'loopne':
            E('if (--CX != 0 && !FZ) %s' % self.goto(a[0][1])); return
        if op == 'jmp':
            t = a[0][1]
            if t in self.fn.insns and not (t == self.fn.off and False):
                E(self.goto(t))
            else:
                E('%s(); return;' % fname(self.fn.space, *near_target(self.fn.space, self.fn.seg, t), 'near'))
            return
        if op == 'call':
            t = a[0][1]
            E('PUSH(0x%04x); %s();' % (nxt, fname(self.fn.space, *near_target(self.fn.space, self.fn.seg, t), 'near'))); return
        if op == 'callf':
            s, t = a[0][1], a[0][2]
            E('PUSH(0x%04x); PUSH(0x%04x); %s' % (self.cs, nxt, farcall(s, t))); return
        if op == 'jmpf':
            s, t = a[0][1], a[0][2]
            E('%s return;' % farcall(s, t)); return
        if op == 'calli':
            E('{ uint16_t t_ = %s; PUSH(0x%04x); call_near(0x%04x, t_); }' % (self.rd(a[0]), nxt, self.cs)); return
        if op == 'callfi':
            s, o = self.ea(a[0])
            E('{ uint16_t o_ = RW(%s, %s), s_ = RW(%s, (uint16_t)(%s + 2)); PUSH(0x%04x); PUSH(0x%04x); call_far(s_, o_); }'
              % (s, o, s, o, self.cs, nxt)); return
        if op == 'jmpi':
            tb = self.fn.tables.get(ins.addr)
            if tb:
                S = self.fn.table_seg.get(ins.addr)
                if S is None:
                    rd = self.rd(a[0])
                else:
                    es, eo = self.ea(a[0])
                    rd = 'RW(0x%04x, %s)' % ((S + LOADSEG) & 0xFFFF, eo)
                E('switch (%s) {' % rd)
                for w, t in sorted(set(tb)):
                    if t in self.fn.insns:
                        E('  case 0x%04x: goto L_%04x;' % (w, t))
                E('  default: rt_badjump(0x%04x, 0x%04x, %s); return; }' % (self.cs, ins.addr, rd))
            else:
                E('call_near(0x%04x, %s); return;' % (self.cs, self.rd(a[0])))
            return
        if op == 'jmpfi':
            s, o = self.ea(a[0])
            E('call_far(RW(%s, (uint16_t)(%s + 2)), RW(%s, %s)); return;' % (s, o, s, o)); return
        if op == 'ret':
            n = a[0][1] if a else 0
            E('POP(); %s%sreturn;' % (('SP += %d; ' % n) if n else '', 'RETCHK(); ' if getattr(self, 'framed', False) else '')); return
        if op == 'retf':
            n = a[0][1] if a else 0
            E('POP(); POP(); %s%sreturn;' % (('SP += %d; ' % n) if n else '', 'RETCHK(); ' if getattr(self, 'framed', False) else '')); return
        if op == 'iret':
            E('POP(); POP(); set_flags(POP()); return;'); return
        raise NotImplementedError('%s at %s:%04x' % (op, self.fn.name, ins.addr))

    def gen(self):
        fn = self.fn
        self.out.append('void %s(void) {' % fn.name)
        self.out.append('    cur_fn = "%s"; SPCHK(); COV();' % fn.name)
        first = fn.insns.get(fn.off)
        self.framed = bool(first and first.op == 'push' and first.args and first.args[0][0] == 'r16'
                           and R16[first.args[0][1]] == 'bp')
        if self.framed:
            self.out.append('    RETCHK_ENTER();')
        if fn.off not in fn.labels:
            pass
        order = sorted(fn.insns)
        if order and order[0] != fn.off:
            self.out.append('    goto L_%04x;' % fn.off)
            fn.labels.add(fn.off)
        prev_end = None
        for idx, o in enumerate(order):
            ins = fn.insns[o]
            self.out.append('L_%04x:' % o)
            hook = HOOKS.get((fn.space.name, fn.seg, o))
            if hook:
                self.out.append('    %s();' % hook)
            self.cur = o
            try:
                self.gen_ins(ins)
            except NotImplementedError as e:
                self.emit('rt_unimpl(0x%04x, 0x%04x); return; /* %s */' % (self.cs, o, e))
            last = ins
            prev_end = (o + ins.length) & 0xFFFF
            if last.op not in ('jmp', 'ret', 'retf', 'iret', 'jmpf', 'jmpi', 'jmpfi', 'hlt'):
                if prev_end not in fn.insns:
                    self.emit('%s(); return; /* fallthrough */' % fname(fn.space, fn.seg, prev_end, 'near'))
                elif idx + 1 >= len(order) or order[idx + 1] != prev_end:
                    fn.labels.add(prev_end)
                    self.emit('goto L_%04x;' % prev_end)
        self.out.append('}')
        return '\n'.join(self.out)


NEEDED = set()


def fname(space, seg, off, kind):
    NEEDED.add((space.name, seg, off))
    return 'f_%s_%04x_%04x' % (space.name, seg, off)


def farcall(s, t):
    ls = (s - LOADSEG) & 0xFFFF
    sps = spaces_for(ls, t)
    if len(sps) == 1 and sps[0].name == 'root':
        NEEDED.add(('root', ls, t))
        return 'f_root_%04x_%04x();' % (ls, t)
    return 'call_far(0x%04x, 0x%04x);' % (s, t)


def main():
    extra = []
    entries_file = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), V.ENTRIES)
    if os.path.exists(entries_file):
        for line in open(entries_file):
            line = line.split('#')[0].strip()
            if line:
                sp_name, so = line.split()
                s, o = [int(v, 16) for v in so.split(':')]
                extra.append((sp_name, s, o))
    entries = collections.defaultdict(set)
    entries['root'].add((HDR['cs'], HDR['ip']))
    for (n, s, o) in extra:
        entries[n].add((s, o))
    for o in FM_ENTRIES:
        entries['fm'].add((FM_SEG - LOADSEG, o))
    funcs = {}
    rounds = 0

    def add_calls(name, fn):
        added = False
        for c in fn.calls:
            kind, cs, co = c
            if kind in ('near', 'tail'):
                if (cs, co) not in entries[name]:
                    entries[name].add((cs, co)); added = True
            else:
                sps = spaces_for(cs, co)
                if len(sps) > 1:
                    sps = [sp2 for sp2 in sps if far_clean(sp2, cs, co)] or sps
                for sp2 in sps:
                    if (cs, co) not in entries[sp2.name]:
                        entries[sp2.name].add((cs, co)); added = True
        return added

    while True:
        changed = True
        while changed:
            changed = False
            rounds += 1
            for name in list(entries):
                space = spaces[name]
                ent_offsets = collections.defaultdict(set)
                for (s, o) in entries[name]:
                    ent_offsets[s].add(o)
                for (s, o) in list(entries[name]):
                    key = (name, s, o)
                    if key in funcs:
                        continue
                    fn = trace(Func(space, s, o), ent_offsets[s])
                    funcs[key] = fn
                    changed = True
                    add_calls(name, fn)
        final = {}
        for name in entries:
            ent_offsets = collections.defaultdict(set)
            for (s, o) in entries[name]:
                ent_offsets[s].add(o)
            for (s, o) in entries[name]:
                final[(name, s, o)] = trace(Func(spaces[name], s, o), ent_offsets[s])
        if not any([add_calls(k[0], f) for k, f in sorted(final.items())]):
            break
    funcs = final
    print('discovery rounds', rounds, 'functions', len(funcs))
    bad = [k for k, f in funcs.items() if f.bad]
    print('functions with decode problems', len(bad))
    if bad:
        per = collections.Counter(k[0] for k in bad); tot = collections.Counter(k[0] for k in funcs)
        print('  ' + '  '.join('%s %d/%d' % (n, per[n], tot[n]) for n in sorted(per)))
    os.makedirs(OUT, exist_ok=True)
    per_space = collections.defaultdict(list)
    for key, fn in sorted(funcs.items()):
        per_space[key[0]].append(fn)
    written = set()
    for name, fns in per_space.items():
        chunks = [fns[i:i + 150] for i in range(0, len(fns), 150)]
        written |= {'%s_%02d.c' % (name, ci) for ci in range(len(chunks))}
        for ci, ch in enumerate(chunks):
            src = ['#include "../cpu.h"', '#include "protos.h"', '']
            for fn in ch:
                src.append(Gen(fn).gen())
                src.append('')
            open(os.path.join(OUT, '%s_%02d.c' % (name, ci)), 'w').write('\n'.join(src))
    import re as _re
    for f in os.listdir(OUT):
        if _re.match(r'^\w+_\d\d\.c$', f) and f not in written:
            os.remove(os.path.join(OUT, f))
            obj = os.path.join(OUT, '..', '..', 'obj', f[:-2] + '.o')
            if os.path.exists(obj):
                os.remove(obj)
    allnames = set('f_%s_%04x_%04x' % k for k in funcs)
    missing = set('f_%s_%04x_%04x' % k for k in NEEDED) - allnames
    with open(os.path.join(OUT, 'protos.h'), 'w') as f:
        for n in sorted(allnames | missing):
            f.write('void %s(void);\n' % n)
    with open(os.path.join(OUT, 'stubs.c'), 'w') as f:
        f.write('#include "../cpu.h"\n#include "protos.h"\n')
        for n in sorted(missing):
            _, sp_, s, o = n.split('_')
            f.write('void %s(void) { rt_missing("%s", 0x%s, 0x%s); }\n' % (n, sp_, s, o))
    with open(os.path.join(OUT, 'dispatch.c'), 'w') as f:
        f.write('#include "../cpu.h"\n#include "protos.h"\n#include "../dispatch.h"\n')
        for name in sorted(per_space):
            fns = sorted(per_space[name], key=lambda fn: (fn.seg, fn.off))
            f.write('const FnEntry tab_%s[] = {\n' % name)
            for fn in fns:
                f.write('  {0x%04x, 0x%04x, %s},\n' % ((fn.seg + LOADSEG) & 0xFFFF, fn.off, fn.name))
            f.write('  {0, 0, 0}};\n')
        undumped = sorted(n for n, sp in spaces.items() if sp.flags & OVL_COMPRESSED and n not in per_space)
        for name in undumped:
            f.write('const FnEntry tab_%s[] = {\n  {0, 0, 0}};\n' % name)
        FP_LEN = 24
        fps = {}
        for name in sorted(per_space):
            sp = spaces[name]
            if not sp.size:
                continue
            if not (sp.flags & OVL_COMPRESSED) and not any(
                    o.flags & OVL_COMPRESSED and o.linkseg == sp.linkseg for o in spaces.values() if o is not sp):
                continue
            ents = sorted(sp.pos(fn.seg, fn.off) for fn in per_space[name])
            mid = next((e for e in ents if e > sp.size // 2 and e + FP_LEN <= sp.size), 0)
            fps[name] = mid
            f.write('const uint8_t fp_%s[%d] = {%s};\n' % (name, 2 * FP_LEN,
                    ','.join('0x%02x' % b for b in list(sp.data[:FP_LEN]) + list(sp.data[mid:mid + FP_LEN]))))
        f.write('const SpaceTab space_tabs[] = {\n')
        for name in sorted(set(per_space) | set(undumped)):
            sp = spaces[name]
            f.write('  {"%s", %d, 0x%04x, %d, %d, %d, %d, %s, %d, tab_%s},\n' % (
                name, getattr(sp, 'ovl_id', 0), (sp.linkseg + LOADSEG) & 0xFFFF, sp.filesize, sp.fileoff,
                sp.memsize, sp.flags, ('fp_%s' % name) if name in fps else '0', fps.get(name, 0), name))
        f.write('  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};\n')
        f.write('const int fp_len = %d;\n' % FP_LEN)
        f.write('const char pop2_version[] = "%s";\n' % VERSION)
        f.write('const long pop2_exe_size = %d;\n' % V.EXE_SIZE)
        f.write('const uint32_t pop2_fp_off = %d;\n' % V.OVL_DESC)
        f.write('const uint8_t pop2_fp[2] = {0x%02x, 0x%02x};\n' % (EXE[V.OVL_DESC], EXE[V.OVL_DESC + 1]))
        f.write('const ExePatch pop2_patches[] = {\n')
        for off, (old, new) in sorted(getattr(V, 'PATCHES', {}).items()):
            hx = lambda s: ', '.join('0x%s' % s[i:i + 2] for i in range(0, len(s), 2))
            f.write('  {0x%05x, %d, {%s}, {%s}},\n' % (off, len(old) // 2, hx(old), hx(new)))
        f.write('  {0, 0, {0}, {0}}};\n')
        hx = lambda s: ','.join('0x%s' % s[i:i + 2] for i in range(0, len(s), 2))
        f.write('const GameFile pop2_files[] = {\n')
        for name, (size, sha, alts) in getattr(V, 'FILES', {}).items():
            assert len(alts) <= 2, name
            alt = ', '.join('"%s"' % a for a in alts) or '0'
            f.write('  {"%s", {%s}, %d, {%s}},\n' % (name, alt, size, hx(sha)))
        f.write('  {0, {0}, 0, {0}}};\n')
        f.write('const OtherExe pop2_other_exes[] = {\n')
        for pre, what in getattr(V, 'OTHER_EXES', {}).items():
            f.write('  {{%s}, "%s"},\n' % (hx(pre), what))
        f.write('  {{0}, 0}};\n')
        if len(EXE) != V.EXE_SIZE:
            print('WARNING: prince.exe is %d bytes, versions/%s.py says %d' % (len(EXE), VERSION, V.EXE_SIZE))
        if undumped:
            print('compressed overlays without a dump in %s: %s' % (os.path.relpath(DUMP_DIR), ' '.join(undumped)))
    print('missing (stubbed):', len(missing))
    print('written to', os.path.abspath(OUT))


if __name__ == '__main__':
    main()
