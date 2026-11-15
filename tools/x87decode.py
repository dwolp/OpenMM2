#!/usr/bin/env python3
"""Symbolic x87 decoder for Open1560's game.asm (a development aid for the MM1
physics port, docs/mm1physics.md).

Follows ONE path through a function and prints every store as a formula, so
the x87 stack shuffling does not have to be decoded by hand.

  x87decode.py game.asm START END [--names names.txt] [--take LABEL,...]
               [--skip LABEL,...] [--regs ebx=W,esi=C]

START and END are 1-based line numbers. At each conditional jump the path falls
through unless the jump's target label is listed in --take (taken) -- a label
listed in --take is taken every time it is reached. Unconditional jumps are
always followed. --names maps "[base+offset]" operands to names, one per line:
    ebx+1F8 W.dispLat
Registers named with --regs (reg=prefix) give their memory operands that
prefix; every other operand prints as written. Stack slots [ebp-N] become
variables tN (a store prints "let tN = ...").
"""
import re
import struct
import sys

CONSTS = {}


def load_consts(lines):
    """flt_XXXX dd 03F800000r / dbl_XXXX dq 03FE0000000000000r -> numbers."""
    for l in lines:
        m = re.match(r'^((?:flt|dbl|sym)_[0-9A-F]+)\s+d([dq])\s+([0-9A-Fa-f]+)r?\s*$', l.strip())
        if not m:
            continue
        name, kind, hx = m.groups()
        try:
            if kind == 'd':
                v = struct.unpack('>f', bytes.fromhex(hx.lstrip('0').rjust(8, '0')[-8:]))[0]
            else:
                v = struct.unpack('>d', bytes.fromhex(hx.rjust(16, '0')[-16:]))[0]
            CONSTS[name] = '%g' % v
        except Exception:
            pass

MEM = re.compile(r'(?:dword|qword|word|byte)\s+ptr\s+\[([^\]]+)\]')


def parse_args(argv):
    opts = {'names': {}, 'take': set(), 'regs': {}, 'init': {}}
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--names':
            for line in open(argv[i + 1]):
                line = line.split('#')[0].strip()
                if line:
                    k, v = line.split(None, 1)
                    opts['names'][k.upper().replace('H', '') if False else k] = v
            i += 2
        elif a == '--take':
            opts['take'] = set(argv[i + 1].split(','))
            i += 2
        elif a == '--regs':
            for kv in argv[i + 1].split(','):
                k, v = kv.split('=')
                opts['regs'][k] = v
            i += 2
        else:
            pos.append(a)
            i += 1
    return pos, opts


def hexint(s):
    s = s.strip()
    neg = s.startswith('-')
    s = s.lstrip('-')
    if s.endswith('h'):
        v = int(s[:-1], 16)
    else:
        v = int(s)
    return -v if neg else v


def mem_name(op, opts):
    m = MEM.search(op)
    if not m:
        return None
    inner = m.group(1).replace(' ', '')
    mm = re.match(r'^([a-z]{3})([+-])([0-9A-Fa-f]+h?)$', inner)
    if mm:
        reg, sign, off = mm.groups()
        off = hexint(off if off.endswith('h') or off.isdigit() else off + 'h')
        off = -off if sign == '-' else off
        if reg == 'ebp':
            return ('slot', -off if off < 0 else off, 't%X' % (-off if off < 0 else off))
        key = '%s%+d' % (reg, off)
        hk = '%s+%X' % (reg, off) if off >= 0 else key
        if hk in opts['names']:
            return ('field', key, opts['names'][hk])
        pre = opts['regs'].get(reg)
        if pre:
            return ('field', key, '%s.%X' % (pre, off) if off >= 0 else '%s.m%X' % (pre, -off))
        return ('field', key, '[%s%s%X]' % (reg, '+' if off >= 0 else '-', abs(off)))
    mm = re.match(r'^([a-z]{3})$', inner)
    if mm:
        reg = mm.group(1)
        key = '%s+0' % reg
        if key in opts['names']:
            return ('field', key, opts['names'][key])
        pre = opts['regs'].get(reg)
        return ('field', key, '%s.0' % pre if pre else '[%s]' % reg)
    g = inner.replace('?', '').split('@')[0]
    return ('global', inner, CONSTS.get(g, g))


class Machine:
    def __init__(self, opts):
        self.opts = opts
        self.st = []
        self.mem = {}
        self.regs = {}
        self.ver = {}
        self.inputs = 0
        self.flag = None  # last compare (a, b)
        self.ahtest = None

    def push(self, e):
        self.st.append(e)

    def pop(self):
        return self.st.pop()

    def _ensure(self, n):
        # Values left on the stack by code before START: placeholders in0, in1...
        while len(self.st) < n:
            self.st.insert(0, 'in%d' % self.inputs)
            self.inputs += 1

    def sti(self, i):
        self._ensure(i + 1)
        return self.st[-1 - i]

    def load_mem(self, op, integer=False):
        k = mem_name(op, self.opts)
        if k is None:
            return op
        kind, key, name = k
        if key in self.mem:
            v = self.mem[key]
        elif kind == 'slot':
            v = name
        else:
            v = name
        if integer:
            return 'int(%s)' % v
        if op.lstrip().startswith('qword'):
            return v
        return v

    def store_mem(self, op, val, pop):
        k = mem_name(op, self.opts)
        kind, key, name = k
        if kind == 'slot':
            self.ver[name] = self.ver.get(name, 0) + 1
            vname = '%s_%d' % (name, self.ver[name])
            print('  let %s = %s' % (vname, val))
            self.mem[key] = vname
        else:
            print('  %s = %s' % (name, val))
            self.mem[key] = name


ARITH = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/'}


def fmt(a, op, b):
    return '(%s %s %s)' % (a, op, b)


def run(lines, opts):
    m = Machine(opts)
    labels = {}
    for i, l in enumerate(lines):
        mm = re.match(r'^(loc_[0-9A-F]+):', l.strip())
        if mm:
            labels[mm.group(1)] = i
    i = 0
    steps = 0
    pending_cmp = None
    while i < len(lines) and steps < 20000:
        steps += 1
        raw = lines[i]
        i += 1
        s = raw.split(';')[0].strip()
        if not s or s.startswith('loc_') and s.endswith(':'):
            continue
        parts = s.split(None, 1)
        op = parts[0]
        args = [a.strip() for a in parts[1].split(',')] if len(parts) > 1 else []
        # re-join memory operands that contain commas (none expected)
        try:
            if op in ('fld',):
                a = args[0]
                if a.startswith('st'):
                    idx = 0 if a == 'st' else int(re.match(r'st\((\d)\)', a).group(1))
                    m.push(m.sti(idx))
                else:
                    m.push(m.load_mem(a))
            elif op == 'fild':
                m.push(m.load_mem(args[0], integer=True))
            elif op in ('fst', 'fstp'):
                a = args[0]
                if a.startswith('st'):
                    idx = 0 if a == 'st' else int(re.match(r'st\((\d)\)', a).group(1))
                    v = m.sti(0)
                    m._ensure(idx + 1)
                    m.st[-1 - idx] = v
                    if op == 'fstp':
                        m.pop()
                else:
                    v = m.sti(0)
                    m.store_mem(a, v, op == 'fstp')
                    if op == 'fstp':
                        m.pop()
            elif op == 'fxch':
                idx = 1 if not args else int(re.match(r'st\((\d)\)', args[0]).group(1))
                m._ensure(idx + 1)
                m.st[-1], m.st[-1 - idx] = m.st[-1 - idx], m.st[-1]
            elif op in ARITH or op in ('fsubr', 'fdivr'):
                sym = ARITH.get(op) or ('-' if op == 'fsubr' else '/')
                rev = op in ('fsubr', 'fdivr')
                if len(args) == 1 and not args[0].startswith('st'):
                    b = m.load_mem(args[0])
                    a = m.sti(0)
                    m.st[-1] = fmt(b, sym, a) if rev else fmt(a, sym, b)
                elif len(args) == 2:
                    d, sres = args
                    di = 0 if d == 'st' else int(re.match(r'st\((\d)\)', d).group(1))
                    si = 0 if sres == 'st' else int(re.match(r'st\((\d)\)', sres).group(1))
                    a = m.sti(di)
                    b = m.sti(si)
                    m.st[-1 - di] = fmt(b, sym, a) if rev else fmt(a, sym, b)
                else:
                    # form "fadd st, st(1)" handled above; memory forms have 1 arg
                    raise ValueError('arith form')
            elif op in ('faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp'):
                base = op[:-1]
                rev = base in ('fsubr', 'fdivr')
                sym = ARITH.get(base) or ('-' if base == 'fsubr' else '/')
                di = int(re.match(r'st\((\d)\)', args[0]).group(1)) if args and args[0] != 'st' else 1
                a = m.sti(di)
                b = m.sti(0)
                m.st[-1 - di] = fmt(b, sym, a) if rev else fmt(a, sym, b)
                m.pop()
            elif op in ('fimul', 'fiadd', 'fisub', 'fidiv'):
                sym = {'fimul': '*', 'fiadd': '+', 'fisub': '-', 'fidiv': '/'}[op]
                m.st[-1] = fmt(m.sti(0), sym, m.load_mem(args[0], integer=True))
            elif op == 'fchs':
                m.st[-1] = '(-%s)' % m.sti(0)
            elif op == 'fabs':
                m.st[-1] = 'abs(%s)' % m.sti(0)
            elif op == 'fsqrt':
                m.st[-1] = 'sqrt(%s)' % m.sti(0)
            elif op in ('fsin', 'fcos'):
                m.st[-1] = '%s(%s)' % (op[1:], m.sti(0))
            elif op in ('fcom', 'fcomp', 'fcompp'):
                a = m.sti(0)
                if op == 'fcompp':
                    b = m.sti(1)
                elif not args:
                    b = m.sti(1)
                elif args[0].startswith('st'):
                    idx = 0 if args[0] == 'st' else int(re.match(r'st\((\d)\)', args[0]).group(1))
                    b = m.sti(idx)
                else:
                    b = m.load_mem(args[0])
                pending_cmp = (a, b)
                if op in ('fcomp', 'fcompp'):
                    m.pop()
                if op == 'fcompp':
                    m.pop()
            elif op == 'fnstsw':
                pass
            elif op == 'test' and args and args[0] == 'ah':
                m.ahtest = hexint(args[1])
            elif op in ('jz', 'jnz', 'je', 'jne', 'jmp', 'jl', 'jge', 'jle', 'jg', 'jb', 'jae'):
                target = args[0]
                if op == 'jmp':
                    i = labels[target] + 1
                    continue
                cond = None
                if pending_cmp and m.ahtest is not None:
                    a, b = pending_cmp
                    t = m.ahtest
                    nz = {1: '%s < %s', 0x41: '%s <= %s', 0x40: '%s == %s', 5: 'unordered/%s<%s', 0x44: 'unordered'}.get(t, 'flags&%s')
                    # jnz jumps when (ah & t) != 0
                    expr = nz % (a, b) if '%s' in nz else nz
                    cond = expr if op == 'jnz' else 'NOT(%s)' % expr
                taken = target in opts['take']
                print('  # %s %s -> %s%s' % (op, target, 'TAKEN' if taken else 'fall', ('   when ' + cond) if cond else ''))
                if taken:
                    i = labels[target] + 1
            elif op == 'mov':
                d, sres = args
                if d.startswith(('dword', 'word', 'byte')):
                    k = mem_name(d, opts)
                    val = m.regs.get(sres, sres)
                    if k[0] == 'slot':
                        m.ver[k[2]] = m.ver.get(k[2], 0) + 1
                        vn = '%s_%d' % (k[2], m.ver[k[2]])
                        print('  let %s = %s   (int)' % (vn, val))
                        m.mem[k[1]] = vn
                    else:
                        print('  %s = %s' % (k[2], val))
                        m.mem[k[1]] = k[2]
                else:
                    if sres.startswith(('dword', 'word', 'byte')):
                        m.regs[d] = m.load_mem(sres)
                    else:
                        m.regs[d] = m.regs.get(sres, sres)
            elif op == 'or' and len(args) == 2 and args[1] == '-1':
                m.regs[args[0]] = '-1'
            elif op == 'xor' and len(args) == 2 and args[0] == args[1]:
                m.regs[args[0]] = '0'
            elif op in ('call',):
                nm = args[0].replace('?', '').split('@')[0]
                if nm in ('Mag', 'Time', 'Dot', 'Mag2', 'Dist', 'Dist2', 'frand', 'InvMag', 'Length', 'Angle'):
                    m.push('%s()' % nm)
                    print('  call %s -> st0' % args[0])
                else:
                    print('  call %s' % args[0])
            elif op in ('retn', 'ret'):
                print('  return')
                break
            elif op in ('push', 'pop', 'lea', 'add', 'sub', 'cmp', 'inc', 'dec', 'rep', 'movsd', 'imul', 'and', 'shl', 'sar'):
                if op in ('lea',):
                    m.regs[args[0]] = '&' + args[1]
                elif args and args[0] in m.regs:
                    m.regs.pop(args[0], None)
            else:
                print('  ?? %s' % s)
        except Exception as e:  # keep going: show where the model fails
            print('  !! %s  (%s: %s) stack=%s' % (s, type(e).__name__, e, m.st))
    print('final stack:', m.st)


def main():
    pos, opts = parse_args(sys.argv[1:])
    path, start, end = pos[0], int(pos[1]), int(pos[2])
    with open(path, encoding='utf-8', errors='ignore') as f:
        alllines = f.read().split('\n')
    load_consts(alllines)
    run(alllines[start - 1:end], opts)


if __name__ == '__main__':
    main()
