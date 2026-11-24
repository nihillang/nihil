#!/usr/bin/env python3
"""translate.py PROGRAM.x86 MAP OUT.s — Nihil on macOS (Apple silicon): the x86-64 code the compiler
emits, translated into ARM64 assembly that clang assembles with arm64/rt.c (the runtime).

The compiler emits a closed set of instructions (about 180 forms: a stack machine, see
docs/macos-arm64.md), so the code is translated instruction by instruction:
  - registers: rax rcx rdx rsi rdi → x9 x10 x11 x12 x13 (scratch, saved around runtime calls);
    rbx r12 r13 r14 r15 (the optimizer's) → x19 x20 x21 x22 x23 (kept by C); rbp → x29;
    rsp → x28: the x86 stack is emulated exactly (8-byte slots) in its own area, so parameters sit
    where the code expects them; the real sp stays 16-aligned for the calls into the runtime.
  - globals (absolute addresses in the data segment) → _nh_globals_start + offset, through x27;
    constants (rip-relative) → the constants copied after the code (Lconst).
  - runtime calls (the jump table) → glue that passes the registers by the ARM convention to
    the functions of rt.c and keeps every other register; the emulated stack holds the saved
    registers, and the collector scans it (nh_emu_sp … nh_emu_top).
  - flags: cmp/test → cmp/tst, ucomisd → fcmp; the conditions are mapped for each.
MAP is the line "nihil-map RUNTIME_END CODE_END SIZE" the compiler writes with -map.
"""
import os, re, struct, subprocess, sys, tempfile

BASE = 0x400000
HEADER = 0xB0
OFS_GLOBALS = 0x21000
BASE_DATA = 0x40000000

R64 = {'rax': 'x9', 'rcx': 'x10', 'rdx': 'x11', 'rsi': 'x12', 'rdi': 'x13', 'rbx': 'x19', 'r12': 'x20', 'r13': 'x21',
       'r14': 'x22', 'r15': 'x23', 'rbp': 'x29', 'rsp': 'x28'}
R32 = {'eax': 'rax', 'ecx': 'rcx', 'edx': 'rdx', 'esi': 'rsi', 'edi': 'rdi', 'ebx': 'rbx', 'r12d': 'r12', 'r13d': 'r13',
       'r14d': 'r14', 'r15d': 'r15'}
R8 = {'al': 'rax', 'cl': 'rcx', 'dl': 'rdx', 'bl': 'rbx'}
SAVED = ['x9', 'x10', 'x11', 'x12', 'x13']          # caller-saved in ARM: kept by the glue

INT_COND = {'e': 'eq', 'z': 'eq', 'ne': 'ne', 'nz': 'ne', 'l': 'lt', 'le': 'le', 'g': 'gt', 'ge': 'ge',
            'b': 'lo', 'be': 'ls', 'a': 'hi', 'ae': 'hs', 's': 'mi', 'ns': 'pl'}
FP_COND = {'e': 'eq', 'z': 'eq', 'ne': 'ne', 'nz': 'ne', 'a': 'gt', 'ae': 'ge', 'b': 'lt', 'be': 'le',
           'p': 'vs', 'np': 'vc'}

# the runtime routines: (x86 inputs → C arguments, C function, outputs) — layout.inc, SPEC §12
RT = {
    5: (['rax', 'rcx'], 'nh_div', ['rax']), 10: (['rax', 'rcx'], 'nh_mod', ['rax']),
    15: (['rax'], 'nh_put_int', []), 20: (['rax'], 'nh_put_bool', []), 25: (['rax'], 'nh_put_text', []),
    30: (['rdi'], 'nh_exit', []), 35: (['rsi'], 'nh_error', []),
    40: (['rcx'], 'nh_alloc', ['rax']), 45: (['rcx', 'rdx'], 'nh_array_rep', ['rax']),
    50: (['rax', 'rcx'], 'nh_error_index', []), 55: (['rax'], 'nh_write_byte', []), 60: ([], 'nh_read_byte', ['rax']),
    65: (['rax', 'rcx'], 'nh_text_equal', ['rax']), 70: (['rax'], 'nh_err_int', []), 75: (['rax'], 'nh_err_bool', []),
    80: (['rax'], 'nh_err_text', []), 85: (['rax'], 'nh_read_file', ['rax']), 90: (['rax'], 'nh_text', ['rax']),
    95: (['rax', 'rdx'], 'nh_put_int_opt', []), 100: (['rax', 'rdx'], 'nh_put_bool_opt', []),
    105: (['rax', 'rdx'], 'nh_put_text_opt', []), 110: (['rax', 'rdx'], 'nh_err_int_opt', []),
    115: (['rax', 'rdx'], 'nh_err_bool_opt', []), 120: (['rax', 'rdx'], 'nh_err_text_opt', []),
    125: (['rax'], 'nh_read_file_opt', ['rax']), 130: (['rax', 'rcx'], 'nh_concat', ['rax']),
    135: (['rax'], 'nh_sleep', []), 140: (['rax', 'rcx'], 'nh_write_file', []), 145: (['rax', 'rcx'], 'nh_append_file', []),
    150: (['rax', 'rcx', 'rdx'], 'nh_read_at3', ['rax', 'rdx', 'rcx']),
    155: (['rax', 'rcx', 'rdx'], 'nh_write_at', []), 160: (['rax', 'rcx', 'rdx'], 'nh_write_at_ns', []),
    165: (['rax', 'rdx'], 'nh_truncate', []), 170: (['rax', 'rdx'], 'nh_truncate_ns', []),
    175: (['rax'], 'nh_int_text', ['rax']), 180: (['rax', 'rcx'], 'nh_array_concat', ['rax']),
    185: (['rax', 'rcx'], 'nh_net_listen', ['rax']), 190: (['rax'], 'nh_net_accept', ['rax']),
    195: (['rax', 'rcx'], 'nh_fd_read', ['rax']), 200: (['rax', 'rcx'], 'nh_fd_send', ['rax']),
    205: (['rax'], 'nh_fd_close', []), 210: (['rax'], 'nh_syscall', ['rax']),
    215: (['rax', 'rcx'], 'nh_json_end', ['rax']), 220: (['rax', 'rcx', 'rdx'], 'nh_sub', ['rax']),
    225: (['rax', 'rcx', 'rdx'], 'nh_put', ['rax']), 230: (['rax', 'rcx', 'rdx'], 'nh_rec_parse', ['rax']),
    235: (['rax', 'rcx'], 'nh_lines', ['rax']), 240: (['rax', 'rcx'], 'nh_net_connect', ['rax']),
    245: (['rax'], 'nh_protect_slot', []), 250: ([], 'nh_unprotect', []),
    265: (['rax', 'rcx'], 'nh_concat_int', ['rax']),
    255: ([], 'nh_err_message', ['rax']), 260: (['rax'], 'nh_fail', []),
}


class Unsupported(Exception):
    pass


def disassemble(data, start, end):
    """(address, raw bytes, text) of each instruction of data[start:end], by clang + objdump"""
    with tempfile.TemporaryDirectory() as d:
        open(d + '/code.bin', 'wb').write(data[start:end])
        open(d + '/code.s', 'w').write('.text\n.globl _code\n_code:\n.incbin "%s/code.bin"\n' % d)
        subprocess.run(['clang', '-target', 'x86_64-apple-macos', '-c', d + '/code.s', '-o', d + '/code.o'], check=True)
        text = subprocess.run(['objdump', '-d', '--x86-asm-syntax=intel', d + '/code.o'], check=True,
                              capture_output=True, text=True).stdout
    out = []
    for line in text.split('\n'):
        m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}[ \t])+)\s*(.*)$', line)
        if m:
            raw = bytes(int(x, 16) for x in m.group(2).split())
            out.append((BASE + start + int(m.group(1), 16), raw, re.sub(r'\s*#.*$', '', m.group(3)).strip()))
    return out


def branch_target(addr, raw):
    """the target of a relative call or jump (rel8 or rel32), or None"""
    op = raw[0]
    if op in (0xE8, 0xE9):
        return addr + len(raw) + struct.unpack('<i', raw[1:5])[0]
    if op == 0xEB or 0x70 <= op <= 0x7F:
        return addr + len(raw) + struct.unpack('<b', raw[1:2])[0]
    if op == 0x0F and 0x80 <= raw[1] <= 0x8F:
        return addr + len(raw) + struct.unpack('<i', raw[2:6])[0]
    return None


class Translator:
    def __init__(self, data, runtime_end, code_end, with_runtime=False):
        self.with_runtime = with_runtime     # the runtime translated too (translate_rt.py), not rt.c's
        self.data = data
        self.runtime_end = runtime_end
        self.code_end = code_end
        self.const_start = (code_end + 7) & ~7
        self.out = []
        self.used_rt = set()
        self.flags = 'int'
        self.fp_regs_used = False

    # ---- helpers ----
    def emit(self, s):
        self.out.append('    ' + s)

    def imm(self, reg, v):
        v &= (1 << 64) - 1
        if v == 0:
            self.emit(f'mov {reg}, #0')
            return
        first = True
        for k in range(4):
            part = (v >> (16 * k)) & 0xFFFF
            if part or (first and k == 3):
                self.emit(f'{"movz" if first else "movk"} {reg}, #{part}, lsl #{16 * k}')
                first = False
        if first:
            self.emit(f'mov {reg}, #0')

    def reg(self, name):
        if name in R64:
            return R64[name]
        if name in R32:
            return R64[R32[name]]
        if name in R8:
            return R64[R8[name]]
        raise Unsupported('register ' + name)

    def address(self, operand):
        """a memory operand → (base register, offset, index register or None, scale)"""
        m = re.search(r'\[(.*)\]', operand)
        inner = m.group(1).replace(' ', '')
        if re.fullmatch(r'0x[0-9a-f]+', inner):                     # an absolute address: a global
            a = int(inner, 16)
            off = a - (BASE_DATA + OFS_GLOBALS)
            if off < 0:
                raise Unsupported('absolute address below the globals: ' + inner)
            return ('x27', off, None, 1)
        m = re.fullmatch(r'rip([+-]0x[0-9a-f]+)', inner)
        if m:
            raise Unsupported('rip-relative memory access')
        m = re.fullmatch(r'([a-z0-9]+)(?:\+(\d)\*([a-z0-9]+))?([+-]0x[0-9a-f]+|[+-]\d+)?', inner)
        if not m:
            m2 = re.fullmatch(r'([a-z0-9]+)\+([a-z0-9]+)', inner)          # [rax+rcx]
            if m2:
                return (self.reg(m2.group(1)), 0, self.reg(m2.group(2)), 1)
            raise Unsupported('memory operand ' + operand)
        base, scale, index, off = m.group(1), m.group(2), m.group(3), m.group(4)
        o = int(off, 0) if off else 0
        return (self.reg(base), o, self.reg(index) if index else None, int(scale) if scale else 1)

    def addr_reg(self, base, off):
        """x15 = base + off, for an address ARM cannot reach with an immediate. x15 holds nothing
        else: x16 and x17 carry values (a constant to store, an operand), and computing the address
        in them overwrote the value before the store; a local past the 32nd of a method (offset
        below -256) then got its address instead of its value (e.g. a query's sum started there)"""
        if -4095 <= off <= 4095:
            self.add_imm('x15', base, off)
        else:
            self.imm('x15', off)
            self.emit(f'add x15, {base}, x15')
        return 'x15'

    def mem(self, operand, size=8):
        """an ARM memory operand for a load/store of size bytes, computing the address into x15 when needed"""
        base, off, index, scale = self.address(operand)
        if index:
            if off:
                base = self.addr_reg(base, off)
            shift = {1: 0, 2: 1, 4: 2, 8: 3}[scale]
            if shift and shift != {8: 3, 1: 0, 4: 2, 2: 1}[size]:
                self.emit(f'add x17, {base}, {index}, lsl #{shift}')
                return '[x17]'
            return f'[{base}, {index}{", lsl #" + str(shift) if shift else ""}]'
        if 0 <= off <= 4095 * size and off % size == 0:
            return f'[{base}, #{off}]' if off else f'[{base}]'
        if -256 <= off <= 255:
            return ('U', f'[{base}, #{off}]')
        return f'[{self.addr_reg(base, off)}]'

    def load(self, dst, operand, size=8, signed=False):
        m = self.mem(operand, size)
        op = {8: 'ldr', 1: 'ldrb'}[size]
        reg = dst if size == 8 else 'w' + dst[1:]
        if isinstance(m, tuple):
            self.emit(f'{"ldur" if size == 8 else "ldurb"} {reg}, {m[1]}')
        else:
            self.emit(f'{op} {reg}, {m}')

    def store(self, src, operand, size=8):
        m = self.mem(operand, size)
        reg = src if size == 8 else 'w' + src[1:]
        if isinstance(m, tuple):
            self.emit(f'{"stur" if size == 8 else "sturb"} {reg}, {m[1]}')
        else:
            self.emit(f'{"str" if size == 8 else "strb"} {reg}, {m}')

    def add_imm(self, dst, src, v):
        if 0 <= v <= 4095:
            self.emit(f'add {dst}, {src}, #{v}')
        elif -4095 <= v < 0:
            self.emit(f'sub {dst}, {src}, #{-v}')
        else:
            self.imm('x17', v)
            self.emit(f'add {dst}, {src}, x17')

    def operand_value(self, op, scratch='x16'):
        """the value of a source operand in a register (the register itself, or scratch)"""
        op = op.strip()
        if re.fullmatch(r'-?0x[0-9a-f]+|-?\d+', op):
            self.imm(scratch, int(op, 0))
            return scratch
        if '[' in op:
            self.load(scratch, op)
            return scratch
        return self.reg(op)

    def is_mem(self, op):
        return '[' in op

    def const_address(self, target):
        if target >= BASE + self.const_start:
            return f'Lconst', target - (BASE + self.const_start)
        if BASE + self.runtime_end <= target < BASE + self.code_end:
            return f'L_{target:x}', 0
        raise Unsupported(f'rip-relative address {target:#x}')

    def push(self, reg):
        self.emit(f'str {reg}, [x28, #-8]!')

    def pop(self, reg):
        self.emit(f'ldr {reg}, [x28], #8')

    # ---- one instruction ----
    def translate(self, addr, raw, text, nxt):
        parts = text.split(None, 1)
        mn = parts[0]
        ops = [o.strip() for o in re.split(r',(?![^\[]*\])', parts[1])] if len(parts) > 1 else []
        target = branch_target(addr, raw)

        if mn == 'push':
            if re.fullmatch(r'-?0x[0-9a-f]+|-?\d+', ops[0]):      # push imm (sign-extended to 64 bits)
                v = int(ops[0], 0)
                if v > 0x7FFFFFFF:
                    v -= 1 << 32
                self.imm('x16', v)
                self.push('x16')
            elif self.is_mem(ops[0]):
                self.load('x16', ops[0])
                self.push('x16')
            else:
                self.push(self.reg(ops[0]))
            return
        if mn == 'pop':
            self.pop(self.reg(ops[0]))
            return
        if mn == 'call':
            if target < BASE + self.runtime_end and self.with_runtime:
                self.rt_calls.add(target)
                self.emit(f'bl L_{target:x}_c')
            elif target < BASE + self.runtime_end:
                self.call_runtime(target - BASE - HEADER)
            else:
                self.emit(f'bl L_{target:x}')
            return
        if mn == 'ret':
            self.pop('x30')
            self.emit('ret')
            return
        if mn == 'leave':
            self.emit('mov x28, x29')
            self.pop('x29')
            return
        if mn == 'jmp':
            if target < BASE + self.runtime_end and self.with_runtime:
                self.emit(f'b L_{target:x}')
                return
            if target < BASE + self.runtime_end:
                raise Unsupported('jump into the runtime')
            self.emit(f'b L_{target:x}')
            return
        if mn.startswith('j'):
            cond = (FP_COND if self.flags == 'fp' else INT_COND)[mn[1:]]
            if target < BASE + self.runtime_end and self.with_runtime:
                self.emit(f'b.{cond} L_{target:x}')
            elif target < BASE + self.runtime_end:          # a jump to a runtime error (index out of bounds)
                rt = target - BASE - HEADER
                self.used_rt.add(rt)
                self.emit(f'b.{cond} Lrt_{rt}_jump')
            else:
                self.emit(f'b.{cond} L_{target:x}')
            return
        if mn.startswith('set'):
            cond = (FP_COND if self.flags == 'fp' else INT_COND)[mn[3:]]
            self.emit(f'cset {self.reg(ops[0])}, {cond}')
            return
        if mn.startswith('cmov'):
            cond = (FP_COND if self.flags == 'fp' else INT_COND)[mn[4:]]
            d = self.reg(ops[0])
            s = self.operand_value(ops[1])
            self.emit(f'csel {d}, {s}, {d}, {cond}')
            return
        if mn == 'cmp':
            self.flags = 'int'
            if self.is_mem(ops[0]):
                self.load('x16', ops[0])
                a = 'x16'
            else:
                a = self.reg(ops[0])
            b = self.operand_value(ops[1], 'x17')
            self.emit(f'cmp {a}, {b}')
            return
        if mn == 'test':
            self.flags = 'int'
            if self.is_mem(ops[0]):
                self.load('x16', ops[0])
                a = 'x16'
            else:
                a = self.reg(ops[0])
            b = self.operand_value(ops[1], 'x17')
            w = 'w' if ops[0] in R32 or (len(ops) > 1 and ops[1] in R32) else 'x'
            self.emit(f'tst {w + a[1:]}, {w + b[1:]}')
            return
        if mn == 'ucomisd':
            self.flags = 'fp'
            self.emit(f'fcmp d{ops[0][-1]}, d{ops[1][-1]}')
            return
        if mn in ('mov', 'movabs'):
            self.mov(ops)
            return
        if mn == 'movzx':
            d = self.reg(ops[0])
            if self.is_mem(ops[1]):
                self.load(d, ops[1], 1)
            else:
                self.emit(f'and {d}, {self.reg(ops[1])}, #0xff')
            return
        if mn == 'lea':
            d = self.reg(ops[0])
            inner = re.search(r'\[(.*)\]', ops[1]).group(1).replace(' ', '')
            m = re.fullmatch(r'rip([+-]0x[0-9a-f]+)', inner)
            if m:
                t = addr + len(raw) + int(m.group(1), 16)
                label, off = self.const_address(t)
                self.emit(f'adrp {d}, {label}@PAGE')
                self.emit(f'add {d}, {d}, {label}@PAGEOFF')
                if off:
                    self.add_imm(d, d, off)
                return
            base, off, index, scale = self.address(ops[1])
            if index:
                shift = {1: 0, 2: 1, 4: 2, 8: 3}[scale]
                self.emit(f'add {d}, {base}, {index}, lsl #{shift}')
                if off:
                    self.add_imm(d, d, off)
            else:
                self.add_imm(d, base, off)
            return
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'imul', 'shl', 'sar', 'shr'):
            self.arith(mn, ops, nxt)
            return
        if mn in ('inc', 'dec'):
            self.arith('add' if mn == 'inc' else 'sub', [ops[0], '1'], nxt)
            return
        if mn == 'neg':
            d = self.reg(ops[0])
            self.emit(f'neg{"s" if self.consumer(nxt) else ""} {d}, {d}')
            if self.consumer(nxt):
                self.flags = 'int'
            return
        if mn == 'not':
            d = self.reg(ops[0])
            self.emit(f'mvn {d}, {d}')
            return
        if mn == 'btc':
            d = self.reg(ops[0])
            bit = int(ops[1], 0)
            self.imm('x16', 1 << bit)
            self.emit(f'eor {d}, {d}, x16')
            return
        if mn == 'movq':
            a, b = ops
            if a.startswith('xmm') and b.startswith('xmm'):
                self.emit(f'fmov d{a[-1]}, d{b[-1]}')
            elif a.startswith('xmm') and self.is_mem(b):
                self.load('x16', b)
                self.emit(f'fmov d{a[-1]}, x16')
            elif a.startswith('xmm'):
                self.emit(f'fmov d{a[-1]}, {self.reg(b)}')
            elif self.is_mem(a):
                self.emit(f'fmov x16, d{b[-1]}')
                self.store('x16', a)
            else:
                self.emit(f'fmov {self.reg(a)}, d{b[-1]}')
            return
        if mn in ('addsd', 'subsd', 'mulsd', 'divsd'):
            op = {'addsd': 'fadd', 'subsd': 'fsub', 'mulsd': 'fmul', 'divsd': 'fdiv'}[mn]
            self.emit(f'{op} d{ops[0][-1]}, d{ops[0][-1]}, d{ops[1][-1]}')
            return
        if mn == 'cvtsi2sd':
            self.emit(f'scvtf d{ops[0][-1]}, {self.reg(ops[1])}')
            return
        if mn == 'cvttsd2si':
            # x86 gives the smallest INT for NaN and out of range; ARM saturates (and gives 0 for NaN)
            d = self.reg(ops[0])
            s = 'd' + ops[1][-1]
            self.emit(f'fcvtzs {d}, {s}')
            self.imm('x16', 1 << 63)
            self.imm('x17', (1 << 63) - 1)
            self.emit(f'cmp {d}, x17')
            self.emit(f'csel {d}, x16, {d}, eq')
            self.emit(f'fcmp {s}, {s}')
            self.emit(f'csel {d}, x16, {d}, vs')
            return
        if mn in ('int3', 'nop'):
            return
        raise Unsupported(text)

    def consumer(self, nxt):
        """the next instruction reads the flags (a jump, set or move on a condition)"""
        if not nxt:
            return False
        mn = nxt.split(None, 1)[0]
        return (mn.startswith('j') and mn != 'jmp') or mn.startswith('set') or mn.startswith('cmov')

    def mov(self, ops):
        d, s = ops
        if self.is_mem(d):
            size = 1 if d.startswith('byte') else 8
            if re.fullmatch(r'-?0x[0-9a-f]+|-?\d+', s):
                self.imm('x16', int(s, 0))
                v = 'x16'
            else:
                v = self.reg(s)
            self.store(v, d, size)
            return
        dr = self.reg(d)
        if self.is_mem(s):
            self.load(dr, s)
        elif re.fullmatch(r'-?0x[0-9a-f]+|-?\d+', s):
            v = int(s, 0)
            if d in R32:
                v &= 0xFFFFFFFF
            self.imm(dr, v)
        else:
            sr = self.reg(s)
            if d in R32:
                self.emit(f'mov w{dr[1:]}, w{sr[1:]}')
            elif dr != sr:
                self.emit(f'mov {dr}, {sr}')

    def arith(self, mn, ops, nxt):
        flags = self.consumer(nxt)
        if self.is_mem(ops[0]):                      # read, change, write back (add/inc/dec in memory)
            self.load('x14', ops[0])
            b = self.operand_value(ops[1], 'x17')
            op = {'add': 'add', 'sub': 'sub'}[mn]
            self.emit(f'{op}{"s" if flags else ""} x14, x14, {b}')
            self.store('x14', ops[0])
            if flags:
                self.flags = 'int'
            return
        d = self.reg(ops[0])
        w = ops[0] in R32 or ops[0] in R8
        if ops[0] in R8 and mn in ('and', 'or'):
            b = self.reg(ops[1])
            self.emit(f'{"and" if mn == "and" else "orr"} {d}, {d}, {b}')
            return
        if w and mn == 'xor' and ops[1] == ops[0]:
            self.emit(f'mov {d}, #0')
            return
        if w and mn == 'xor':
            v = int(ops[1], 0)
            self.imm('x16', v)
            self.emit(f'eor w{d[1:]}, w{d[1:]}, w16')
            return
        if mn in ('shl', 'sar', 'shr'):
            op = {'shl': 'lsl', 'sar': 'asr', 'shr': 'lsr'}[mn]
            if ops[1] == 'cl':
                self.emit(f'{op} {d}, {d}, x10')
            else:
                self.emit(f'{op} {d}, {d}, #{int(ops[1], 0)}')
            return
        if mn == 'imul':
            if len(ops) == 3:
                s = self.operand_value(ops[1], 'x16')
                self.imm('x17', int(ops[2], 0))
                self.emit(f'mul {d}, {s}, x17')
            else:
                s = self.operand_value(ops[1], 'x16')
                self.emit(f'mul {d}, {d}, {s}')
            return
        s = ops[1]
        if mn in ('add', 'sub') and re.fullmatch(r'-?0x[0-9a-f]+|-?\d+', s) and 0 <= int(s, 0) <= 4095:
            self.emit(f'{mn}{"s" if flags else ""} {d}, {d}, #{int(s, 0)}')
        else:
            b = self.operand_value(s, 'x16')
            op = {'add': 'add', 'sub': 'sub', 'and': 'and', 'or': 'orr', 'xor': 'eor'}[mn]
            if flags and op in ('add', 'sub', 'and'):
                op += 's'
            self.emit(f'{op} {d}, {d}, {b}')
        if flags:
            if mn not in ('add', 'sub', 'and'):
                self.emit(f'cmp {d}, #0')
            self.flags = 'int'

    def call_runtime(self, rt):
        if rt not in RT:
            raise Unsupported(f'runtime routine at {rt}')
        self.used_rt.add(rt)
        self.emit(f'bl Lrt_{rt}')

    def glue(self, rt):
        if rt == 245:                                 # RT_PROTECT: a frame to resume at (rt.c, nh_resume)
            return ['Lrt_245:', '    sub x28, x28, #48',
                    '    stp x9, x10, [x28]', '    stp x11, x12, [x28, #16]', '    stp x13, x30, [x28, #32]',
                    '    adrp x16, _nh_emu_sp@PAGE', '    str x28, [x16, _nh_emu_sp@PAGEOFF]',
                    '    bl _nh_protect_slot',
                    '    ldp x9, x10, [x28]', '    ldp x11, x12, [x28, #16]', '    ldp x13, x30, [x28, #32]',
                    '    add x28, x28, #48',
                    '    str x28, [x0]', '    str x29, [x0, #8]', '    str x9, [x0, #16]',
                    '    stp x19, x20, [x0, #24]', '    stp x21, x22, [x0, #40]', '    str x23, [x0, #56]',
                    '    mov x16, sp', '    str x16, [x0, #64]', '    ret']
        ins, fn, outs = RT[rt]
        L = [f'Lrt_{rt}:', '    sub x28, x28, #48',
             '    stp x9, x10, [x28]', '    stp x11, x12, [x28, #16]', '    stp x13, x30, [x28, #32]',
             '    adrp x16, _nh_emu_sp@PAGE', '    str x28, [x16, _nh_emu_sp@PAGEOFF]']
        for k, r in enumerate(ins):
            L.append(f'    mov x{k}, {R64[r]}')
        if rt == 150:                                 # read_at: the text, then the size and the stamp
            L += ['    bl _nh_read_at', '    mov x9, x0', '    bl _nh_last_size', '    mov x11, x0',
                  '    bl _nh_last_stamp', '    mov x10, x0']
        else:
            L.append(f'    bl _{fn}')
            for k, r in enumerate(outs):
                L.append(f'    mov {R64[r]}, x{k}')
        restore = {'x9': '[x28]', 'x10': '[x28, #8]', 'x11': '[x28, #16]', 'x12': '[x28, #24]', 'x13': '[x28, #32]'}
        for reg, place in restore.items():
            if reg not in [R64[o] for o in outs]:
                L.append(f'    ldr {reg}, {place}')
        L += ['    ldr x30, [x28, #40]', '    add x28, x28, #48', '    ret']
        if rt == 50:                                  # the target of a conditional jump: never returns
            L += [f'Lrt_{rt}_jump:', '    mov x0, x9', '    mov x1, x10', '    bl _nh_error_index']
        return L

    def translate_runtime(self):
        """the runtime's code (from the header to rt_data) in ARM64, by translate_rt.py"""
        import translate_rt as TR
        self.rt_data_off = HEADER + struct.unpack_from('<I', self.data, self.runtime_end - 4)[0]
        if not HEADER < self.rt_data_off < self.runtime_end:
            sys.exit('translate: the runtime does not say where its data starts (an older runtime?)')
        insns = disassemble(self.data, HEADER, self.rt_data_off)
        rt = TR.RuntimeTranslator(self, BASE + HEADER, BASE + self.rt_data_off, BASE + self.runtime_end)
        targets, calls = set(), set()
        for addr, raw, text in insns:
            t = branch_target(addr, raw)
            if t is not None:
                targets.add(t)
                if raw[0] == 0xE8:
                    calls.add(t)
        # the program's calls into the runtime (its jump table) are found while the program is translated:
        # every routine gets its call label, so any of them may be called
        lines, errors = [], []
        for i, (addr, raw, text) in enumerate(insns):
            lines.append(f'L_{addr:x}:')
            rt.out = []
            try:
                rt.translate(addr, raw, text, [insns[k][2] for k in range(i - 1, max(i - 8, -1), -1)],
                             insns[i + 1][2] if i + 1 < len(insns) else '',
                             TR.needs_flags(insns, i, targets), branch_target(addr, raw),
                             lambda t: f'L_{t:x}_c', lambda t: f'L_{t:x}')
            except TR.Unsupported as e:
                errors.append(f'{addr:#x}: {text}   ({e})')
            lines.extend(rt.out)
        if errors:
            sys.stderr.write('translate: %d runtime instructions not supported:\n  %s\n' % (len(errors), '\n  '.join(errors[:40])))
            sys.exit(1)
        self.rt_insn_addrs = [a for a, _, _ in insns]
        self.rt_call_targets = calls
        self.rt_translator = rt
        return lines

    def runtime_call_stubs(self):
        """L_x_c for each routine called: the return address on the x86 stack, then the routine"""
        out = []
        for t in sorted(self.rt_call_targets | self.rt_calls):
            out += [f'L_{t:x}_c:', '    str x30, [x28, #-8]!', f'    b L_{t:x}']
        if self.rt_translator.uses_sys:
            import translate_rt as TR
            out += TR.glue_sys()
        return out

    def run(self, out_path):
        self.rt_calls = set()
        rt_lines = self.translate_runtime() if self.with_runtime else []
        insns = disassemble(self.data, self.runtime_end, self.code_end)
        targets, entries = set(), set()
        for addr, raw, text in insns:
            t = branch_target(addr, raw)
            if t is not None and t >= BASE + self.runtime_end:
                targets.add(t)
                if raw[0] == 0xE8:
                    entries.add(t)
            m = re.match(r'lea\s+\w+,\s*\[rip\s*([+-])\s*(0x[0-9a-f]+)\]', text)
            if m:                                     # a code address taken (where a protected call resumes)
                a = addr + len(raw) + int(m.group(1) + m.group(2), 16)
                if BASE + self.runtime_end <= a < BASE + self.code_end:
                    targets.add(a)
        errors = []
        body = []
        for i, (addr, raw, text) in enumerate(insns):
            if addr in targets:
                body.append(f'L_{addr:x}:')
                if addr in entries:                    # a method: its return address goes on the x86 stack
                    body.append('    str x30, [x28, #-8]!')
            self.out = []
            try:
                self.translate(addr, raw, text, insns[i + 1][2] if i + 1 < len(insns) else '')
            except Unsupported as e:
                errors.append(f'{addr:#x}: {text}   ({e})')
            body.extend(self.out)
        if errors:
            sys.stderr.write('translate: %d instructions not supported:\n  %s\n' % (len(errors), '\n  '.join(errors[:30])))
            sys.exit(1)
        # the data segment's size: the globals
        phoff = struct.unpack_from('<Q', self.data, 32)[0]
        memsz = struct.unpack_from('<Q', self.data, phoff + 56 + 40)[0]
        globals_size = max(memsz - OFS_GLOBALS, 8)
        consts = self.data[self.const_start:]
        s = ['// generated by macos/translate.py from the x86-64 code of the Nihil compiler — do not edit',
             '    .text', '    .p2align 2', '    .globl _main', '_main:',
             '    stp x29, x30, [sp, #-16]!', '    mov x29, sp',
             '    bl _nh_emu_stack', '    mov x28, x0', '    mov x29, x0',
             '    adrp x27, _nh_globals_start@PAGE', '    add x27, x27, _nh_globals_start@PAGEOFF',
             f'    b L_{BASE + self.runtime_end:x}']
        if BASE + self.runtime_end not in targets:
            body.insert(0, f'L_{BASE + self.runtime_end:x}:')
        if self.with_runtime:                        # the platform's part: the stack's top, the data's end
            s.insert(s.index(f'    b L_{BASE + self.runtime_end:x}'), '\n'.join([
                f'    movz x17, #{(OFS_GLOBALS - 0x6C8) & 0xFFFF}', f'    movk x17, #{(OFS_GLOBALS - 0x6C8) >> 16}, lsl #16',
                '    sub x16, x27, x17', '    str x28, [x16]',
                '    adrp x17, _nh_globals_end@PAGE', '    add x17, x17, _nh_globals_end@PAGEOFF',
                '    str x17, [x16, #8]']))
        s += body
        s += rt_lines
        if self.with_runtime:
            s += self.runtime_call_stubs()
        for rt in sorted(self.used_rt):
            s += self.glue(rt)
        if self.with_runtime:
            s += ['    .data', '    .p2align 4', 'Lrtdata:']
            rd = self.data[self.rt_data_off:self.runtime_end]
            for k in range(0, len(rd), 32):
                s.append('    .byte ' + ', '.join(str(b) for b in rd[k:k + 32]))
            s += ['    .p2align 4', '_nh_data_start:', f'    .space {OFS_GLOBALS}']   # the runtime's slots and buffers
        s += ['    .data', '    .p2align 4', '    .globl _nh_globals_start, _nh_globals_end', '_nh_globals_start:',
              f'    .space {globals_size}', '_nh_globals_end:', '    .p2align 4', 'Lconst:']
        for k in range(0, len(consts), 32):
            s.append('    .byte ' + ', '.join(str(b) for b in consts[k:k + 32]))
        open(out_path, 'w').write('\n'.join(s) + '\n')
        return len(insns)


def main():
    prog, mapline, out = sys.argv[1], sys.argv[2], sys.argv[3]
    m = re.search(r'nihil-map (\d+) (\d+) (\d+)', open(mapline).read())
    if not m:
        sys.exit('translate: no "nihil-map" line in ' + mapline)
    t = Translator(open(prog, 'rb').read(), int(m.group(1)), int(m.group(2)),
                   os.environ.get('NIHIL_MAC_RUNTIME') == 'asm')
    n = t.run(out)
    sys.stderr.write(f'translate: {n} instructions\n')


if __name__ == '__main__':
    main()
