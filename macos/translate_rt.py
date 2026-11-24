"""translate_rt.py — the Linux runtime (src/runtime.asm, hand-written x86-64) translated into ARM64 for
macOS, used by translate.py with --runtime. The Mac then runs the same runtime as Linux: the collector,
the files, the texts; only the kernel differs, and arm64/rt.c turns each Linux system call into macOS's
(nh_linux_syscall).

Unlike the compiler's code (a closed set of forms), the runtime uses any instruction, so each one is
translated with its exact meaning:
  - registers as translate.py maps them, and r8 r9 r10 → x24 x25 x26 (kept by C), r11 → x7 (a Linux
    system call destroys r11 anyway); 32-bit operations on w registers (they zero the upper half, as
    on x86); 8- and 16-bit writes change only those bits (bfi).
  - memory: [base + index*scale + disp]; an absolute address in the data segment (the runtime's slots
    below the globals, the buffers) is relative to x27, as the globals are; so is an immediate that is
    such an address (mov rsi, OUTPUT_BUF).
  - flags: an instruction that sets them does so in ARM only when a later instruction reads them. A
    compare, a subtraction and a negation map the conditions as translate.py does (ARM's carry is the
    inverse of x86's after a subtraction, and the conditions are chosen for that); a logical operation
    is followed by cmp #0, which gives the same N, Z, V = 0 and "no borrow". A carry read after an
    addition, an inc or a dec is refused, and so is any flag read across a call, a ret or a jump: those
    cannot be translated faithfully, and the runtime does not do them (translation stops if it ever does).
  - calls: each routine has two labels, L_x for jumps and L_x_c for calls, which pushes the return
    address on the x86 stack (x28) as call does; ret pops it. bl and ret stay paired.
  - syscall → Lsys: the Linux registers (rax, rdi, rsi, rdx, r10, r8, r9) to nh_linux_syscall.
  - rep movs/stos, repne scas, rep cmps → loops; div after xor edx, edx and idiv after cqo → udiv/sdiv
    (anything else is refused: a 128-bit dividend is not translated).
"""
import re

BASE = 0x400000
BASE_DATA = 0x40000000
OFS_GLOBALS = 0x21000
GLOBALS = BASE_DATA + OFS_GLOBALS
DATA_LIMIT = BASE_DATA + 0x10000000          # an absolute value up to here is an address in the data

X64 = {'rax': 'x9', 'rcx': 'x10', 'rdx': 'x11', 'rsi': 'x12', 'rdi': 'x13', 'rbx': 'x19', 'r12': 'x20',
       'r13': 'x21', 'r14': 'x22', 'r15': 'x23', 'rbp': 'x29', 'rsp': 'x28',
       'r8': 'x24', 'r9': 'x25', 'r10': 'x26', 'r11': 'x7'}
R32 = {'eax': 'rax', 'ecx': 'rcx', 'edx': 'rdx', 'esi': 'rsi', 'edi': 'rdi', 'ebx': 'rbx', 'ebp': 'rbp',
       'r8d': 'r8', 'r9d': 'r9', 'r10d': 'r10', 'r11d': 'r11', 'r12d': 'r12', 'r13d': 'r13', 'r14d': 'r14',
       'r15d': 'r15'}
R16 = {'ax': 'rax', 'cx': 'rcx', 'dx': 'rdx', 'bx': 'rbx', 'si': 'rsi', 'di': 'rdi'}
R8 = {'al': 'rax', 'cl': 'rcx', 'dl': 'rdx', 'bl': 'rbx', 'sil': 'rsi', 'dil': 'rdi',
      'r8b': 'r8', 'r9b': 'r9', 'r10b': 'r10', 'r11b': 'r11', 'r12b': 'r12', 'r13b': 'r13', 'r14b': 'r14',
      'r15b': 'r15'}

# conditions after a compare/subtraction (and after a logical operation followed by cmp #0)
COND = {'e': 'eq', 'z': 'eq', 'ne': 'ne', 'nz': 'ne', 'l': 'lt', 'le': 'le', 'g': 'gt', 'ge': 'ge',
        'nl': 'ge', 'ng': 'le', 'nge': 'lt', 'nle': 'gt',
        'b': 'lo', 'c': 'lo', 'nae': 'lo', 'be': 'ls', 'na': 'ls', 'a': 'hi', 'nbe': 'hi',
        'ae': 'hs', 'nb': 'hs', 'nc': 'hs', 's': 'mi', 'ns': 'pl', 'o': 'vs', 'no': 'vc'}
CARRY = {'b', 'c', 'nae', 'be', 'na', 'a', 'nbe', 'ae', 'nb', 'nc'}
SIGNED_CMP = {'l', 'le', 'g', 'ge', 'nl', 'ng', 'nge', 'nle'}
FP = re.compile(r'^(?:j|set|cmov)')

IMM = re.compile(r'^-?(?:0x[0-9a-f]+|\d+)$')


class Unsupported(Exception):
    pass


class CodeHeader(Exception):
    """a read of the executable's ELF header, which the Mac's translation does not have: the code that
    does it runs only on Linux (the Mac gives PLAT_DATA_END), so it becomes a trap"""


def size_of(op):
    m = re.match(r'(byte|word|dword|qword) ptr', op)
    return {'byte': 1, 'word': 2, 'dword': 4, 'qword': 8}[m.group(1)] if m else None


def reg_size(name):
    if name in X64: return 8
    if name in R32: return 4
    if name in R16: return 2
    if name in R8: return 1
    return None


def flag_reader(mn):
    return (mn.startswith('j') and mn != 'jmp') or mn.startswith('set') or mn.startswith('cmov')


def cond_of(mn):
    for p in ('set', 'cmov', 'j'):
        if mn.startswith(p):
            return mn[len(p):]
    return None


class RuntimeTranslator:
    """translates the instructions of the runtime's code, one at a time, into ARM64 lines"""

    def __init__(self, base_translator, rt_start, rt_data, rt_end):
        self.t = base_translator           # translate.py's Translator: imm(), labels of the program
        self.rt_start = rt_start           # addresses (with BASE) of the runtime's code, data and end
        self.rt_data = rt_data
        self.rt_end = rt_end
        self.out = []
        self.kind = None                   # what set the flags: 'sub', 'add', 'incdec', 'logic', 'ovf', 'bit', 'zero'
        self.uses_sys = False
        self.local = 0

    # ---- output ----
    def emit(self, s):
        self.out.append('    ' + s)

    def imm(self, reg, v):
        self.t.out = []
        self.t.imm(reg, v)
        self.out.extend(self.t.out)

    def label(self):
        self.local += 1
        return f'Lrl{self.local}'

    # ---- registers ----
    def x(self, name):
        """the 64-bit ARM register under any x86 name"""
        for table in (R32, R16, R8):
            if name in table:
                name = table[name]
        if name in X64:
            return X64[name]
        raise Unsupported('register ' + name)

    @staticmethod
    def w(xr):
        return 'w' + xr[1:]

    def write_reg(self, name, value):
        """a value (an x register, in its low bits) into the x86 register name, by its size"""
        d = self.x(name)
        n = reg_size(name)
        if n == 8:
            if value != d:
                self.emit(f'mov {d}, {value}')
        elif n == 4:
            self.emit(f'mov {self.w(d)}, {self.w(value)}')        # zeroes the upper half, as x86 does
        elif n == 2:
            self.emit(f'bfi {d}, {value}, #0, #16')
        else:
            self.emit(f'bfi {d}, {value}, #0, #8')

    # ---- memory ----
    def address(self, op):
        """the address of a memory operand → (base x register, index x register or None, shift, disp)"""
        inner = re.search(r'\[(.*)\]', op).group(1).replace(' ', '')
        inner = re.sub(r'^[a-z]s:', '', inner)
        if inner.startswith('rip'):
            raise Unsupported('rip-relative memory (lea it first): ' + op)
        terms = re.findall(r'([+-]?)([^+-]+)', inner)
        base = index = None
        shift = 0
        disp = 0
        for sign, term in terms:
            m = re.fullmatch(r'(\d)\*(\w+)', term)
            if m:
                index, shift = self.x(m.group(2)), {1: 0, 2: 1, 4: 2, 8: 3}[int(m.group(1))]
            elif IMM.match(term):
                disp += int(sign + term, 0)
            elif base is None:
                if sign == '-':
                    raise Unsupported('negative register in ' + op)
                base = self.x(term)
            elif index is None:
                index = self.x(term)
            else:
                raise Unsupported('memory operand ' + op)
        if BASE_DATA <= disp < DATA_LIMIT:            # in the data segment: relative to x27
            disp -= GLOBALS
            if base is None:
                base = 'x27'
            elif index is None:
                index, shift = base, 0
                base = 'x27'
            else:
                self.emit(f'add x15, x27, {base}')
                base = 'x15'
        elif base is None and index is None:
            if BASE <= disp < BASE + 0x1000:
                raise CodeHeader(op)                  # the ELF header: read only on Linux (PLAT_DATA_END)
            raise Unsupported('absolute address outside the data: ' + op)
        elif base is None:
            base = index
            index = None
            if shift:
                raise Unsupported('scaled index without a base: ' + op)
        return base, index, shift, disp

    def mem(self, op, size):
        """an ARM addressing for a load or store of size bytes"""
        base, index, shift, disp = self.address(op)
        lg = {1: 0, 2: 1, 4: 2, 8: 3}[size]
        if index is not None:
            if disp:
                self.add_imm('x15', base, disp)
                base = 'x15'
            if shift and shift != lg:
                self.emit(f'add x15, {base}, {index}, lsl #{shift}')
                return f'[x15]', False
            return f'[{base}, {index}{", lsl #" + str(shift) if shift else ""}]', False
        if disp == 0:
            return f'[{base}]', False
        if 0 <= disp <= 4095 * size and disp % size == 0:
            return f'[{base}, #{disp}]', False
        if -256 <= disp <= 255:
            return f'[{base}, #{disp}]', True                       # ldur/stur
        self.add_imm('x15', base, disp)
        return '[x15]', False

    def add_imm(self, d, s, v):
        if 0 <= v <= 4095:
            self.emit(f'add {d}, {s}, #{v}')
        elif -4095 <= v < 0:
            self.emit(f'sub {d}, {s}, #{-v}')
        else:
            self.imm('x17', v)
            self.emit(f'add {d}, {s}, x17')

    def load(self, dst_x, op, size, signed=False):
        """dst_x (an x register) = the size bytes at op, zero-extended (or sign-extended)"""
        m, u = self.mem(op, size)
        ins = {1: 'ldrb', 2: 'ldrh', 4: 'ldr', 8: 'ldr'}[size]
        if signed and size < 8:
            ins = {1: 'ldrsb', 2: 'ldrsh', 4: 'ldrsw'}[size]
            reg = dst_x
        else:
            reg = dst_x if size == 8 else self.w(dst_x)
        if u:
            ins = ins.replace('ldr', 'ldur')
        self.emit(f'{ins} {reg}, {m}')

    def store(self, src_x, op, size):
        m, u = self.mem(op, size)
        ins = {1: 'strb', 2: 'strh', 4: 'str', 8: 'str'}[size]
        reg = src_x if size == 8 else self.w(src_x)
        if u:
            ins = ins.replace('str', 'stur')
        self.emit(f'{ins} {reg}, {m}')

    # ---- operands ----
    def value(self, op, size, scratch, signed=False):
        """an operand's value in an x register (zero-extended to its size, or sign-extended)"""
        op = op.strip()
        if IMM.match(op):
            v = int(op, 0)
            if BASE_DATA <= v < DATA_LIMIT and size == 8:
                self.add_imm(scratch, 'x27', v - GLOBALS)       # an address in the data segment
                return scratch
            if size < 8:
                v &= (1 << (8 * size)) - 1
                if signed and v >> (8 * size - 1):
                    v -= 1 << (8 * size)
            self.imm(scratch, v)
            return scratch
        if '[' in op:
            self.load(scratch, op, size, signed)
            return scratch
        xr = self.x(op)
        n = reg_size(op)
        if n == 8:
            return xr
        if op == 'ah':
            raise Unsupported('ah')
        ext = {4: 'w', 2: 'h', 1: 'b'}[n]
        if signed:
            self.emit(f'sxt{ext} {scratch}, {self.w(xr)}' if n < 4 else f'sxtw {scratch}, {self.w(xr)}')
        elif n == 4:
            self.emit(f'mov {self.w(scratch)}, {self.w(xr)}')
        else:
            self.emit(f'uxt{ext} {self.w(scratch)}, {self.w(xr)}')
        return scratch

    def op_size(self, ops):
        for o in ops:
            n = size_of(o) if '[' in o else reg_size(o.strip())
            if n:
                return n
        return 8

    # ---- flags ----
    def set_kind(self, k):
        self.kind = k

    def cond(self, c, mn_text):
        k = self.kind
        if k is None:
            raise Unsupported('flags read with no instruction that set them before: ' + mn_text)
        if k == 'ovf':
            if c not in ('o', 'no'):
                raise Unsupported('after imul only o/no: ' + mn_text)
            return 'ne' if c == 'o' else 'eq'
        if k == 'bit':
            m = {'b': 'ne', 'c': 'ne', 'ae': 'eq', 'nc': 'eq', 'nb': 'eq'}
            if c not in m:
                raise Unsupported('after bt only b/ae: ' + mn_text)
            return m[c]
        if k == 'zero' and c not in ('e', 'z', 'ne', 'nz'):
            raise Unsupported('only e/ne here: ' + mn_text)
        if k in ('add', 'incdec') and c in CARRY:
            if k == 'add' and c in ('b', 'c', 'nae'):
                return 'cs'
            if k == 'add' and c in ('ae', 'nb', 'nc'):
                return 'cc'
            raise Unsupported('a carry read after an addition, inc or dec: ' + mn_text)
        return COND[c]

    # ---- one instruction ----
    def translate(self, addr, raw, text, prev, nxt, needs_flags, target, call_label, jump_label):
        """prev: the instructions before (nearest first). needs_flags: a later instruction reads the
        flags this one sets. target: a branch's address."""
        try:
            self.translate1(addr, raw, text, prev, nxt, needs_flags, target, call_label, jump_label)
        except CodeHeader:
            self.emit('brk #0x6e68')                  # never reached on the Mac: stop if it ever is

    def translate1(self, addr, raw, text, prev, nxt, needs_flags, target, call_label, jump_label):
        parts = text.split(None, 1)
        mn = parts[0]
        rest = parts[1] if len(parts) > 1 else ''
        if mn in ('rep', 'repne', 'repe'):
            self.string_op(mn, rest)
            return
        ops = [o.strip() for o in re.split(r',(?![^\[]*\])', rest)] if rest else []
        n = self.op_size(ops)
        F = needs_flags

        if mn == 'syscall':
            self.uses_sys = True
            self.emit('bl Lsys')
            self.kind = None
            return
        if mn == 'call':
            self.emit(f'bl {call_label(target)}')
            self.kind = None
            return
        if mn == 'ret':
            self.emit('ldr x30, [x28], #8')
            self.emit('ret')
            self.kind = None
            return
        if mn == 'jmp':
            if '[' in rest:                              # jmp [mem]: an address the program gave (an ARM label)
                self.load('x16', rest, 8)
                self.emit('br x16')
            else:
                self.emit(f'b {jump_label(target)}')
            return
        if flag_reader(mn) and mn.startswith('j'):
            self.emit(f'b.{self.cond(cond_of(mn), text)} {jump_label(target)}')
            return
        if mn.startswith('set'):
            c = self.cond(cond_of(mn), text)
            self.emit(f'cset w16, {c}')
            self.write_reg(ops[0], 'x16')
            return
        if mn.startswith('cmov'):
            c = self.cond(cond_of(mn), text)
            s = self.value(ops[1], n, 'x16')
            d = self.x(ops[0])
            if n == 4:
                self.emit(f'csel {self.w(d)}, {self.w(s)}, {self.w(d)}, {c}')
            else:
                self.emit(f'csel {d}, {s}, {d}, {c}')
            return
        if mn == 'push':
            v = int(ops[0], 0) if IMM.match(ops[0]) else None
            if v is not None:
                if v > 0x7FFFFFFF:
                    v -= 1 << 32
                self.imm('x16', v)
                s = 'x16'
            else:
                s = self.value(ops[0], 8, 'x16')
            self.emit(f'str {s}, [x28, #-8]!')
            return
        if mn == 'pop':
            self.emit(f'ldr {self.x(ops[0])}, [x28], #8')
            return
        if mn in ('mov', 'movabs'):
            self.mov(ops)
            return
        if mn == 'movzx':
            src_n = size_of(ops[1]) if '[' in ops[1] else reg_size(ops[1])
            v = self.value(ops[1], src_n, 'x16')
            self.write_reg(ops[0], v)
            return
        if mn == 'movsxd' or mn == 'movsx':
            src_n = size_of(ops[1]) if '[' in ops[1] else reg_size(ops[1])
            v = self.value(ops[1], src_n, 'x16', signed=True)
            self.write_reg(ops[0], v)
            return
        if mn == 'lea':
            self.lea(addr, raw, ops)
            return
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test', 'adc', 'sbb'):
            self.alu(mn, ops, n, F, nxt)
            return
        if mn in ('inc', 'dec'):
            self.alu('add' if mn == 'inc' else 'sub', [ops[0], '1'], n, F, nxt, incdec=True)
            return
        if mn == 'neg':
            self.unary(ops[0], n, lambda d: f'neg{"s" if F else ""} {d}, {d}')
            if F:
                self.set_kind('sub')
            return
        if mn == 'not':
            self.unary(ops[0], n, lambda d: f'mvn {d}, {d}')
            return
        if mn in ('shl', 'shr', 'sar', 'sal'):
            self.shift(mn, ops, n, F)
            return
        if mn == 'imul':
            self.imul(ops, n, F, nxt)
            return
        if mn == 'mul':
            s = self.value(ops[0], 8, 'x16')
            self.emit(f'umulh x17, x9, {s}')
            self.emit(f'mul x9, x9, {s}')
            self.emit('mov x11, x17')
            self.kind = None
            return
        if mn in ('div', 'idiv'):
            # the dividend's upper half (rdx) must be known: the last instruction before that writes
            # rdx is xor edx, edx (div) or cqo (idiv); then a 64-bit division is exact
            setter = None
            for p in prev:
                q = p.replace(' ', '').replace('\t', '')
                if q in ('xoredx,edx', 'xorrdx,rdx', 'cqo'):
                    setter = q
                    break
                if re.match(r'\w+\s+(rdx|edx|dx|dl)\b', p) or p.split()[0] in ('call', 'syscall', 'mul', 'div', 'idiv', 'pop'):
                    break
                if p.split()[0] in ('jmp', 'ret') or flag_reader(p.split()[0]) and p.startswith('j'):
                    break
            if mn == 'div' and setter not in ('xoredx,edx', 'xorrdx,rdx'):
                raise Unsupported('div without xor edx, edx before it: ' + text)
            if mn == 'idiv' and setter != 'cqo':
                raise Unsupported('idiv without cqo before it: ' + text)
            s = self.value(ops[0], 8, 'x16')
            op = 'udiv' if mn == 'div' else 'sdiv'
            self.emit(f'{op} x17, x9, {s}')
            self.emit(f'msub x11, x17, {s}, x9')
            self.emit('mov x9, x17')
            self.kind = None
            return
        if mn == 'cqo':
            self.emit('asr x11, x9, #63')
            return
        if mn == 'bsr':
            s = self.value(ops[1], 8, 'x16')
            if F:
                self.emit(f'cmp {s}, #0')
                self.set_kind('zero')
            self.emit(f'clz x17, {s}')
            self.emit('mov x16, #63')
            self.emit(f'sub {self.x(ops[0])}, x16, x17')
            return
        if mn in ('bt', 'bts', 'btr', 'btc'):
            d = self.x(ops[0])
            if not IMM.match(ops[1]):
                raise Unsupported('bt with a register bit: ' + text)
            bit = int(ops[1], 0)
            self.imm('x16', 1 << bit)
            if F:
                self.emit(f'tst {d}, x16')
                self.set_kind('bit')
            if mn == 'bts':
                self.emit(f'orr {d}, {d}, x16')
            elif mn == 'btr':
                self.emit(f'bic {d}, {d}, x16')
            elif mn == 'btc':
                self.emit(f'eor {d}, {d}, x16')
            return
        if mn == 'bswap':
            d = self.x(ops[0])
            if reg_size(ops[0]) == 4:
                self.emit(f'rev {self.w(d)}, {self.w(d)}')
            else:
                self.emit(f'rev {d}, {d}')
            return
        if mn == 'xchg':
            if sorted(ops) == ['ah', 'al']:
                self.emit('rev16 w16, w9')
                self.emit('bfi x9, x16, #0, #16')
                return
            a, b = self.x(ops[0]), self.x(ops[1])
            if reg_size(ops[0]) == 8 and reg_size(ops[1]) == 8:
                self.emit(f'mov x16, {a}')
                self.emit(f'mov {a}, {b}')
                self.emit(f'mov {b}, x16')
                return
            raise Unsupported(text)
        if mn.startswith('prefetch'):
            base, index, shift, disp = self.address(ops[0])
            if index is None and 0 <= disp <= 32760 and disp % 8 == 0:
                self.emit(f'prfm pldl1keep, [{base}, #{disp}]')
            return
        if mn == 'cvtsi2sd':
            self.emit(f'scvtf d{ops[0][3:]}, {self.value(ops[1], 8, "x16")}')
            return
        if mn in ('divsd', 'mulsd', 'addsd', 'subsd'):
            arm = {'divsd': 'fdiv', 'mulsd': 'fmul', 'addsd': 'fadd', 'subsd': 'fsub'}[mn]
            d = 'd' + ops[0][3:]
            if '[' in ops[1]:
                m, u = self.mem(ops[1], 8)
                self.emit(f'{"ldur" if u else "ldr"} d16, {m}')
                s = 'd16'
            else:
                s = 'd' + ops[1][3:]
            self.emit(f'{arm} {d}, {d}, {s}')
            return
        if mn == 'movq':
            a, b = ops
            if a.startswith('xmm') and not b.startswith('xmm') and '[' not in b:
                self.emit(f'fmov d{a[3:]}, {self.x(b)}')
            elif b.startswith('xmm') and not a.startswith('xmm') and '[' not in a:
                self.emit(f'fmov {self.x(a)}, d{b[3:]}')
            else:
                raise Unsupported(text)
            return
        if mn in ('nop', 'int3', 'endbr64'):
            return
        if mn in ('clc', 'stc', 'cmc'):
            raise Unsupported('the carry as an answer (rewrite the runtime to use a register): ' + text)
        raise Unsupported(text)

    def mov(self, ops):
        d, s = ops
        if '[' in d:
            n = size_of(d) or reg_size(s)
            if IMM.match(s):
                v = int(s, 0)
                if n == 8 and BASE_DATA <= v < DATA_LIMIT:
                    self.add_imm('x16', 'x27', v - GLOBALS)
                else:
                    if n < 8:
                        v &= (1 << (8 * n)) - 1
                    elif v > 0x7FFFFFFF or v < -0x80000000:
                        pass
                    self.imm('x16', v)
                self.store('x16', d, n)
            else:
                if s == 'ah':
                    raise Unsupported('ah')
                self.store(self.x(s), d, n)
            return
        n = reg_size(d)
        if '[' in s:
            if n == 8 or n == 4:
                self.load(self.x(d), s, n)
            else:
                self.load('x16', s, n)
                self.write_reg(d, 'x16')
            return
        if IMM.match(s):
            v = int(s, 0)
            if n in (4, 8) and BASE_DATA <= v < DATA_LIMIT:
                # an address in the data segment (nasm writes mov rdi, OUTPUT_BUF as mov edi, …: 32 bits
                # zero the upper half, so it is the whole address): in the Mac's data area. The runtime
                # has no other number in that range (checked when this was written).
                self.add_imm(self.x(d), 'x27', v - GLOBALS)
                return
            if n == 4:
                v &= 0xFFFFFFFF
            if n >= 4:
                self.imm(self.x(d), v)
            else:
                self.imm('x16', v)
                self.write_reg(d, 'x16')
            return
        v = self.value(s, reg_size(s), 'x16')
        self.write_reg(d, v)

    def lea(self, addr, raw, ops):
        d = ops[0]
        inner = re.search(r'\[(.*)\]', ops[1]).group(1).replace(' ', '')
        m = re.fullmatch(r'rip([+-]0x[0-9a-f]+)', inner)
        if m:
            target = addr + len(raw) + int(m.group(1), 16)
            label, off = self.data_or_code_label(target)
            self.emit(f'adrp x16, {label}@PAGE')
            self.emit(f'add x16, x16, {label}@PAGEOFF')
            if off:
                self.add_imm('x16', 'x16', off)
            self.write_reg(d, 'x16')
            return
        inner_terms = re.findall(r'([+-]?)([^+-]+)', inner)
        if any(re.fullmatch(r'\d\*\w+', t) for _, t in inner_terms) and not any(
                not re.fullmatch(r'\d\*\w+', t) and not IMM.match(t) for _, t in inner_terms):
            # [scale*index + disp], no base
            idx = next(t for _, t in inner_terms if '*' in t)
            k, r = idx.split('*')
            disp = sum(int(sg + t, 0) for sg, t in inner_terms if IMM.match(t))
            self.emit(f'lsl x16, {self.x(r)}, #{ {1: 0, 2: 1, 4: 2, 8: 3}[int(k)] }')
            if disp:
                self.add_imm('x16', 'x16', disp)
            self.write_reg(d, 'x16')
            return
        base, index, shift, disp = self.address(ops[1])
        if index is not None:
            self.emit(f'add x16, {base}, {index}{", lsl #" + str(shift) if shift else ""}')
            if disp:
                self.add_imm('x16', 'x16', disp)
        else:
            self.add_imm('x16', base, disp)
        self.write_reg(d, 'x16')

    def data_or_code_label(self, target):
        if self.rt_data <= target < self.rt_end:
            return 'Lrtdata', target - self.rt_data
        if self.rt_start <= target < self.rt_data:
            return f'L_{target:x}', 0
        return self.t.const_address(target)

    def unary(self, op, n, fmt):
        if '[' in op:
            self.load('x14', op, n)
            d = 'x14' if n == 8 else 'w14'
            self.emit(fmt(d))
            self.store('x14', op, n)
            return
        xr = self.x(op)
        if n == 8:
            self.emit(fmt(xr))
        elif n == 4:
            self.emit(fmt(self.w(xr)))
        else:
            raise Unsupported('8/16-bit ' + fmt('?'))

    def alu(self, mn, ops, n, F, nxt, incdec=False):
        dst = ops[0]
        signed = False
        if F and nxt:
            c = cond_of(nxt.split()[0]) if flag_reader(nxt.split()[0]) else None
            signed = c in SIGNED_CMP or c in ('s', 'ns', 'o', 'no')
        if mn in ('cmp', 'test') or '[' in dst or n < 4:
            # operate in scratch registers: a in x14, b in x16/x17
            if '[' in dst:
                self.load('x14', dst, n, signed and n < 8 and mn in ('cmp',))
                a = 'x14'
            else:
                a = self.value(dst, n, 'x14', signed and mn == 'cmp')
            b = self.value(ops[1], n, 'x17', signed and mn == 'cmp')
            wa = (lambda r: r) if (n == 8 or (signed and n < 8 and mn == 'cmp')) else self.w
            if mn == 'cmp':
                self.emit(f'cmp {wa(a)}, {wa(b)}')
                self.set_kind('sub')
                return
            if mn == 'test':
                if a == b:
                    self.emit(f'cmp {wa(a)}, #0')
                else:
                    self.emit(f'and x16, {a}, {b}')
                    self.emit(f'cmp {wa("x16")}, #0')
                self.set_kind('logic')
                return
            if a != 'x14':
                self.emit(f'mov x14, {a}')
            op = {'add': 'add', 'sub': 'sub', 'and': 'and', 'or': 'orr', 'xor': 'eor'}.get(mn)
            if op is None:
                raise Unsupported(mn + ' on memory or a byte')
            ww = self.w if n < 8 else (lambda r: r)
            s = 's' if F and mn in ('add', 'sub') and n >= 4 else ''
            self.emit(f'{op}{s} {ww("x14")}, {ww("x14")}, {ww(b)}')
            if '[' in dst:
                self.store('x14', dst, n)
            else:
                self.write_reg(dst, 'x14')
            if F:
                if mn in ('add', 'sub') and n >= 4:
                    self.set_kind('incdec' if incdec else mn)
                else:
                    if n < 4:
                        self.emit(f'tst w14, #{(1 << (8 * n)) - 1}')
                        self.emit('cmp w14, w14')    # placeholder: byte flags are not used by the runtime
                        raise Unsupported('flags of an 8/16-bit operation')
                    self.emit(f'cmp {ww("x14")}, #0')
                    self.set_kind('logic')
            return
        d = self.x(dst)
        ww = self.w if n == 4 else (lambda r: r)
        if mn == 'xor' and ops[1] == dst:
            self.emit(f'mov {ww(d)}, #0' if n == 4 else f'mov {d}, #0')
            if F:
                self.emit(f'cmp {d}, #0')
                self.set_kind('logic')
            return
        s1 = ops[1]
        if mn in ('add', 'sub') and IMM.match(s1) and 0 <= int(s1, 0) <= 4095:
            self.emit(f'{mn}{"s" if F else ""} {ww(d)}, {ww(d)}, #{int(s1, 0)}')
        elif mn in ('add', 'sub') and IMM.match(s1) and -4095 <= int(s1, 0) < 0:
            other = 'sub' if mn == 'add' else 'add'
            if F:
                raise Unsupported('flags of an add/sub of a negative immediate')
            self.emit(f'{other} {ww(d)}, {ww(d)}, #{-int(s1, 0)}')
        else:
            b = self.value(s1, n, 'x16')
            op = {'add': 'add', 'sub': 'sub', 'and': 'and', 'or': 'orr', 'xor': 'eor'}[mn]
            if F and mn in ('add', 'sub'):
                op += 's'
            self.emit(f'{op} {ww(d)}, {ww(d)}, {ww(b)}')
        if F:
            if mn in ('add', 'sub'):
                self.set_kind('incdec' if incdec else mn)
            else:
                self.emit(f'cmp {ww(d)}, #0')
                self.set_kind('logic')

    def shift(self, mn, ops, n, F):
        op = {'shl': 'lsl', 'sal': 'lsl', 'shr': 'lsr', 'sar': 'asr'}[mn]
        if '[' in ops[0]:
            raise Unsupported('shift in memory')
        d = self.x(ops[0])
        ww = self.w if n == 4 else (lambda r: r)
        if n < 4:
            raise Unsupported('8/16-bit shift')
        amount = ops[1] if len(ops) > 1 else '1'
        if amount == 'cl':
            self.emit(f'{op} {ww(d)}, {ww(d)}, {ww("x10")}')
        else:
            k = int(amount, 0) & (63 if n == 8 else 31)
            if k:
                self.emit(f'{op} {ww(d)}, {ww(d)}, #{k}')
        if F:
            self.emit(f'cmp {ww(d)}, #0')
            self.set_kind('zero')

    def imul(self, ops, n, F, nxt):
        if n != 8:
            raise Unsupported('32-bit imul')
        d = self.x(ops[0])
        if len(ops) == 3:
            a = self.value(ops[1], 8, 'x14')
            b = self.value(ops[2], 8, 'x17')
        else:
            a = d
            b = self.value(ops[1], 8, 'x17')
        if F:
            self.emit(f'smulh x16, {a}, {b}')
            self.emit(f'mul {d}, {a}, {b}')
            self.emit(f'cmp x16, {d}, asr #63')           # overflow: the high half is not the sign
            self.set_kind('ovf')
        else:
            self.emit(f'mul {d}, {a}, {b}')

    def string_op(self, prefix, rest):
        """rep movsb/movsq/stosb/stosq, repne scasb, rep(e) cmpsb: loops on rcx (x10), rsi (x12), rdi (x13)"""
        op = rest.split()[0]
        end = self.label()
        top = self.label()
        self.emit(f'cbz x10, {end}')
        self.out.append(f'{top}:')
        if op in ('movsb', 'movsq'):
            k, r = (1, 'w16') if op == 'movsb' else (8, 'x16')
            ld, st = ('ldrb', 'strb') if k == 1 else ('ldr', 'str')
            self.emit(f'{ld} {r}, [x12], #{k}')
            self.emit(f'{st} {r}, [x13], #{k}')
            self.emit('sub x10, x10, #1')
            self.emit(f'cbnz x10, {top}')
        elif op in ('stosb', 'stosq'):
            if op == 'stosb':
                self.emit('strb w9, [x13], #1')
            else:
                self.emit('str x9, [x13], #8')
            self.emit('sub x10, x10, #1')
            self.emit(f'cbnz x10, {top}')
        elif op == 'scasb' and prefix == 'repne':
            self.emit('ldrb w16, [x13], #1')
            self.emit('sub x10, x10, #1')
            self.emit('and w17, w9, #0xff')
            self.emit('cmp w17, w16')
            self.emit(f'b.eq {end}')
            self.emit(f'cbnz x10, {top}')
            self.set_kind('zero')
        elif op == 'cmpsb' and prefix in ('rep', 'repe'):
            self.emit('ldrb w16, [x12], #1')
            self.emit('ldrb w17, [x13], #1')
            self.emit('sub x10, x10, #1')
            self.emit('cmp w16, w17')
            self.emit(f'b.ne {end}')
            self.emit(f'cbnz x10, {top}')
            self.set_kind('zero')
        else:
            raise Unsupported(prefix + ' ' + rest)
        self.out.append(f'{end}:')


def glue_sys():
    """Lsys: a Linux system call (rax = number, rdi rsi rdx r10 r8 r9) by nh_linux_syscall; rcx and r11
    are destroyed, as by the kernel, every other register kept"""
    return ['Lsys:',
            '    stp x30, x11, [sp, #-32]!',
            '    stp x12, x13, [sp, #16]',
            '    mov x0, x9', '    mov x1, x13', '    mov x2, x12', '    mov x3, x11',
            '    mov x4, x26', '    mov x5, x24', '    mov x6, x25',
            '    bl _nh_linux_syscall',
            '    mov x9, x0',
            '    ldp x12, x13, [sp, #16]',
            '    ldp x30, x11, [sp], #32',
            '    ret']


def needs_flags(insns, i, targets):
    """the flags instruction i sets are read later: the instructions after it, through those that leave
    the flags as they are, up to one that reads them (True) or sets them, calls, returns or jumps (False)"""
    transparent = ('mov', 'movabs', 'movzx', 'movsx', 'movsxd', 'lea', 'push', 'pop', 'nop', 'prefetcht0',
                   'cqo', 'bswap', 'not', 'xchg', 'movq', 'cvtsi2sd', 'divsd')
    for j in range(i + 1, min(i + 40, len(insns))):
        mn = insns[j][2].split()[0]
        if flag_reader(mn):
            return True
        if mn in transparent:
            continue
        return False
    return False
