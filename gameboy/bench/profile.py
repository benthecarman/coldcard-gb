#!/usr/bin/env python3
#
# Turn bench PC samples into a per-source-line profile.
#
#   profile.py ELF < samples        (lines "pc=ADDR n=COUNT" from bench)
#
import collections, subprocess, sys

elf = sys.argv[1]
samples = []
for line in sys.stdin:
    if line.startswith('pc='):
        a, n = line.split()
        samples.append((int(a[3:]), int(n[2:])))

total = sum(n for _, n in samples)
addrs = ['0x%x' % (a + 8) for a, _ in samples]          # middle of the bucket
out = subprocess.run(['arm-none-eabi-addr2line', '-e', elf, '-f', '-i', '-s'] + addrs,
                     capture_output=True, text=True).stdout.split('\n')

# addr2line -i prints the inline chain per address: func/line pairs; it ends
# each address's chain where the next address starts, which we can't see,
# so ask one at a time for exactness when the sample count is small
by_func = collections.Counter()
by_line = collections.Counter()
for (a, n) in samples:
    r = subprocess.run(['arm-none-eabi-addr2line', '-e', elf, '-f', '-i', '-s', '0x%x' % (a + 8)],
                       capture_output=True, text=True).stdout.split('\n')
    func, where = r[0], r[1]
    by_func[func] += n
    by_line[func + ' ' + where] += n

print('%d samples' % total)
print('\nby function (innermost inlined):')
for f, n in by_func.most_common(25):
    print('%5.1f%%  %s' % (100.0 * n / total, f))
print('\nby line:')
for f, n in by_line.most_common(30):
    print('%5.1f%%  %s' % (100.0 * n / total, f))
