#!/usr/bin/env python3
"""Compare hosted-timer suite runs side by side (markdown).

    summarize.py <label>=<run-suite out dir> ...
"""
import os
import re
import sys


def read(path):
    try:
        with open(path, errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def ticklag(text):
    rows = {}
    for line in text.splitlines():
        m = re.match(r'(\w+)\s+host\s+(\d+) ms.*lost\s+(-?\d+) ms\s+aros/host\s+([\d.]+)%', line)
        if m:
            rows[m.group(1)] = '%s%% (%s ms lost)' % (m.group(4), m.group(3))
            continue
        m = re.match(r'(lat\w+)\s+want\s+(\d+) us\s+min\s+(\d+)\s+median\s+(\d+)\s+p90\s+(\d+)\s+max\s+(\d+)\s+mean\s+(\d+)\s+early (\d+) of (\d+)', line)
        if m:
            rows[m.group(1)] = '%.1f–%.1f ms, mean %.1f, early %s/%s' % (
                int(m.group(3)) / 1000, int(m.group(6)) / 1000, int(m.group(7)) / 1000, m.group(8), m.group(9))
    return rows


def cunit(text):
    tests = re.findall(r'^TEST: (\S+)', text, re.M)
    chunks = re.split(r'^TEST: \S+.*$', text, flags=re.M)[1:]
    res = {}
    for name, chunk in zip(tests, chunks):
        m = re.search(r'Tests\s*:\s*(\d+)\s+(\d+)', chunk)
        a = re.search(r'Asserts\s*:\s*(\d+)\s+(\d+)', chunk)
        res[name] = ('%s/%s tests failed, %s/%s asserts' % (m.group(2), m.group(1), a.group(2) if a else '?',
                                                          a.group(1) if a else '?')) if m else 'no summary'
    return res


def first(text, pattern, default='—'):
    m = re.search(pattern, text, re.M)
    return m.group(0).strip() if m else default


def main():
    runs = []
    for arg in sys.argv[1:]:
        label, d = arg.split('=', 1)
        runs.append((label, d))
    data = {}
    for label, d in runs:
        r = {}
        tl = ticklag(read(os.path.join(d, 'ticklag.txt')))
        for k, v in tl.items():
            r['ticklag ' + k] = v
        fr = read(os.path.join(d, 'frequencies.txt'))
        r['misc/frequencies'] = first(fr, r'FAIL.*') if 'FAIL' in fr else ('pass' if 'EClock' in fr else 'no output')
        r['timer/getsystime 10'] = first(read(os.path.join(d, 'getsystime.txt')), r'result.*')
        cl = read(os.path.join(d, 'clock.txt'))
        r['crt/stdc/clock'] = ' '.join(l.strip() for l in cl.splitlines() if re.search(r'Expected|Got', l)) or '—'
        for p in ('usleep', 'childwait', 'childstatus'):
            r[p + ' rc'] = first(read(os.path.join(d, p + '.txt')), r'^rc \S+$')
        r['ticklag rc'] = first(read(os.path.join(d, 'ticklag.txt')), r'^rc \S+$')
        cu = cunit(read(os.path.join(d, 'aros.log')))
        bad = {k: v for k, v in cu.items() if not v.startswith('0/')}
        r['CUnit'] = '%d programs, %d not clean' % (len(cu), len(bad))
        for k, v in bad.items():
            r['CUnit ' + k] = v
        steps = re.findall(r'^## step (\S+)', read(os.path.join(d, 'aros.log')), re.M)
        r['last step started'] = steps[-1] if steps else '—'
        r['suite elapsed'] = first(read(os.path.join(d, 'elapsed.txt')), r'elapsed.*')
        r['timeout'] = 'yes' if 'timeout after' in read(os.path.join(d, 'aros.log')) else 'no'
        data[label] = r
    keys = []
    for label, _ in runs:
        for k in data[label]:
            if k not in keys:
                keys.append(k)
    print('| | ' + ' | '.join(l for l, _ in runs) + ' |')
    print('| --- |' + ' --- |' * len(runs))
    for k in keys:
        print('| %s | %s |' % (k, ' | '.join(data[l].get(k, '—') for l, _ in runs)))


if __name__ == '__main__':
    main()
