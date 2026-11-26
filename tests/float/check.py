# Checks Nihil's FLOAT ⇄ text conversions against Python (repr = the shortest form).
# Usage: check.py <conversion_check binary> [number of random cases]
import random, struct, subprocess, sys, math
def bits(x): return struct.unpack('<q', struct.pack('<d', x))[0]
def from_bits(b): return struct.unpack('<d', struct.pack('<q', b))[0]
def significant(t):
    t = t.lstrip('-').lower()
    m = t.split('e')[0].replace('.', '').lstrip('0').rstrip('0')
    return len(m) or 1
random.seed(42)
cases = [0.1, 0.2, 0.3, 1.0, 3.0, 2.5, 1e21, 1e20, 1e-7, 1e-6, 123456789.0, 5e-324, 2.2250738585072014e-308,
         2.225073858507201e-308, 1.7976931348623157e308, 9007199254740992.0, 9007199254740993.0, 0.5, 2**-1074,
         4.35, 1/3, 2/3, 100.0, 1e22, 1e23, 8.41e21, 5e-310, 1.5e300, 123.456, 0.000123, 299792458.0]
for p in range(-330, 310): cases.append(10.0 ** p) if abs(p) < 308 else None
for e in range(-1074, 1024, 7): cases.append(2.0 ** e)
for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 20000):
    cases.append(from_bits(random.getrandbits(63)) if random.random() < .6 else random.uniform(-1e6, 1e6))
# what data holds: whole numbers and 1 to 3 decimals (prices, measures), where FLOAT → text
# takes its fast way (float_text.nihil, __nihil_ft_fast)
for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 20000):
    cases.append(random.randint(-10**random.randint(1, 15), 10**random.randint(1, 15)) / random.choice([1, 10, 100, 1000]))
# and 4 to 17 decimals, ratios and small values, where the fast way now goes on to 17 decimals
for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 20000):
    cases.append(random.randint(-10**random.randint(1, 17), 10**random.randint(1, 17)) / 10 ** random.randint(4, 17))
    cases.append(random.random() * 10 ** random.randint(-8, 8))
    cases.append(random.randint(1, 10**6) / random.randint(1, 10**6))
cases = [c for c in cases if c is not None and math.isfinite(c)]
cases += [-c for c in cases[:200]]
stdin = ''.join(f'{bits(c)} {repr(c)}\n' for c in cases)
r = subprocess.run([sys.argv[1]], input=stdin.encode(), capture_output=True)
if r.returncode != 0: print('failed:', r.returncode, r.stderr.decode()[:300]); sys.exit(1)
lines = r.stdout.decode().split('\n')
errors = 0
for c, l in zip(cases, lines):
    text, parsed, same = l.split(' ')
    round_trip = float(text) == c and math.copysign(1, float(text)) == math.copysign(1, c)
    shortest = significant(text) == significant(repr(c))
    exact_parse = int(parsed) == bits(c)
    same = same == '1'                      # the fast way and the general one agree
    if not (round_trip and shortest and exact_parse and same):
        errors += 1
        if errors <= 10: print(f'ERROR {c!r}: nihil="{text}" parsed={parsed} expected={bits(c)} round_trip={round_trip} shortest={shortest} parse={exact_parse} same={same}')
print(f'{len(cases)} cases, {errors} errors')
sys.exit(1 if errors else 0)
