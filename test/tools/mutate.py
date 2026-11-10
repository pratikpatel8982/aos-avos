#!/usr/bin/env python3
"""Mutation audit for the C unit tests: do the tests notice when the production code is broken?

    python3 test/tools/mutate.py test/unit/audio/test_downmix.c [--source Source/downmix.c] [-n 80] [-j 2]

For one test file this

  1. builds the test against the unmodified source and records which source lines the tests execute
     (mutating code no test runs would only produce meaningless survivors),
  2. generates single-edit mutants of those lines (flip a comparison, && <-> ||, + <-> -, * <-> /,
     off-by-one on a constant, negate an if/while condition, delete a statement),
  3. compiles every mutant, runs every TEST of the file against it (each in its own process, with a
     timeout, because mutants like to loop forever), and records which tests failed.

A mutant that makes no test fail is a SURVIVOR: an assertion is missing or too weak. A test that fails
on no mutant at all is reported as KILLED NOTHING: a candidate for deletion or for a better assertion.
Mutants that do not compile are discarded. The source tree is never modified; mutants live in a scratch dir.

Needs build-cov/ (make -C test coverage-lib) and gcovr. Results go to --out (JSON + text summary).
"""
import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TEST_DIR = REPO / 'test'
COV = Path(os.environ.get('COV_DIR', REPO / 'build-cov'))
FIXTURES = TEST_DIR / 'fixtures'


# ---------------------------------------------------------------- build / run

def gcc_cmd(flags, test_c, extra_src, out, scratch_inc=None):
    cmd = [os.environ.get('REAL_CC', 'gcc'), *flags['cflags'], '-O0', '-g', '--coverage', '-fprofile-update=atomic',
           '-ffunction-sections', '-fdata-sections', '-w', f'-DAVOS_TEST_FIXTURES="{FIXTURES}"']
    if scratch_inc:
        cmd.append('-I' + str(scratch_inc))
    cmd += ['-I' + str(TEST_DIR / 'support'), '-I' + str(REPO), str(test_c)]
    if extra_src:
        cmd.append(str(extra_src))
    cmd += [str(COV / 'libavos_cov.a'), *flags['libs'], '-Wl,--gc-sections', '-o', str(out)]
    return cmd


def compile_ok(cmd):
    return subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=300).returncode == 0


def list_tests(binary):
    out = subprocess.run([str(binary), '--list'], cwd=REPO, capture_output=True, text=True, timeout=30).stdout
    return [l.strip() for l in out.splitlines() if l.strip()]


def run_test(binary, name, timeout):
    env = dict(os.environ, GCOV_ERROR_FILE='/dev/null')
    try:
        return subprocess.run([str(binary), name], cwd=REPO, env=env, capture_output=True, timeout=timeout).returncode == 0
    except subprocess.TimeoutExpired:
        return False


# ---------------------------------------------------------------- mutation operators

def mask(line):
    """Blank out string/char literals and comments so operators inside them are not mutated."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c in '"\'':
            q, j = c, i + 1
            while j < n and line[j] != q:
                j += 2 if line[j] == '\\' else 1
            out.append(' ' * (min(j, n - 1) - i + 1))
            i = j + 1
        elif line.startswith('//', i) or line.startswith('/*', i):
            out.append(' ' * (n - i))
            break
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def balanced_end(s, open_idx):
    depth = 0
    for k in range(open_idx, len(s)):
        if s[k] == '(':
            depth += 1
        elif s[k] == ')':
            depth -= 1
            if depth == 0:
                return k
    return -1


def mutants_for_line(raw):
    """Yield (kind, new_line) for every single edit of one source line."""
    m = mask(raw)
    stripped = m.strip()
    if not stripped or stripped.startswith('#'):
        return
    for tok, rep in (('==', '!='), ('!=', '=='), ('<=', '<'), ('>=', '>'), ('&&', '||'), ('||', '&&')):
        for mt in re.finditer(re.escape(tok), m):
            yield f'{tok}->{rep}', raw[:mt.start()] + rep + raw[mt.end():]
    for mt in re.finditer(r'(?<![<>\-=!&|])([<>])(?![<>=\-])', m):
        yield f'{mt.group(1)}->{mt.group(1)}=', raw[:mt.start()] + mt.group(1) + '=' + raw[mt.end():]
    for mt in re.finditer(r'(?<=[\w\)\]])\s*([+\-*/])(?![+\-=>])\s*(?=[\w\(])', m):
        op = mt.group(1)
        pos = mt.start(1)
        rep = {'+': '-', '-': '+', '*': '/', '/': '*'}[op]
        if op == '-' and m[pos:pos + 2] == '->':
            continue
        yield f'{op}->{rep}', raw[:pos] + rep + raw[pos + 1:]
    for mt in re.finditer(r'(?<![\w.\'"])(\d+)(?![\w.xX])', m):
        v = int(mt.group(1))
        for nv in {v + 1, max(v - 1, 0)} - {v}:
            yield f'{v}->{nv}', raw[:mt.start()] + str(nv) + raw[mt.end():]
    for mt in re.finditer(r'\b(if|while)\s*\(', m):
        op = mt.end() - 1
        cl = balanced_end(m, op)
        if cl > 0:
            yield f'negate {mt.group(1)}', raw[:op + 1] + '!(' + raw[op + 1:cl] + ')' + raw[cl:]
    if re.match(r'^\s*[\w\->.\[\]*()]+\s*(=|\+=|-=|\|=|&=)[^=].*;\s*$', m) or \
       re.match(r'^\s*(?!return|if|while|for|switch)\w+\s*\(.*\)\s*;\s*$', m):
        indent = re.match(r'\s*', raw).group(0)
        yield 'delete statement', indent + ';'


def executed_lines(flags, test_c, source, white_box, scratch):
    """Lines of `source` that this test file's tests execute (per-file coverage, not the aggregate)."""
    base = scratch / 'base'
    base.mkdir()
    binary = base / 'bin'
    cmd = gcc_cmd(flags, test_c, None if white_box else source, binary)
    if not compile_ok(cmd):
        sys.exit('baseline build failed:\n' + subprocess.run(cmd, cwd=REPO, capture_output=True, text=True).stderr[-3000:])
    for t in list_tests(binary):
        run_test(binary, t, 60)
    rel = source.relative_to(REPO)
    js = subprocess.run(['gcovr', '-r', str(REPO), '--object-directory', str(base), '--filter', re.escape(str(source)),
                         '--json', '-'], capture_output=True, text=True, cwd=REPO).stdout
    lines = set()
    for f in json.loads(js)['files']:
        if f['file'].endswith(str(rel)):
            lines |= {l['line_number'] for l in f['lines'] if l['count'] > 0}
    return binary, lines


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('test_c')
    ap.add_argument('--source', help='production file to mutate (default Source/<name>.c from test_<name>.c)')
    ap.add_argument('-n', type=int, default=80, help='max mutants to try (random sample, fixed seed)')
    ap.add_argument('-j', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=15, help='seconds per test per mutant')
    ap.add_argument('--out', default=str(REPO / 'build-cov' / 'mutation'))
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()

    test_c = Path(a.test_c).resolve()
    name = test_c.stem[len('test_'):]
    source = (REPO / a.source) if a.source else REPO / 'Source' / f'{name}.c'
    if not source.exists():
        sys.exit(f'no production source {source}; pass --source')
    flags = json.loads((COV / 'flags.json').read_text())
    libdirs = [f[2:] for f in flags['libs'] if f.startswith('-L')]
    os.environ['LD_LIBRARY_PATH'] = os.pathsep.join(libdirs + [os.environ.get('LD_LIBRARY_PATH', '')]).strip(os.pathsep)
    white_box = f'"Source/{source.name}"' in test_c.read_text()
    out_dir = Path(a.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    scratch = Path(tempfile.mkdtemp(prefix=f'mut-{name}-', dir=os.environ.get('MUT_TMP', '/tmp')))
    try:
        binary, hit = executed_lines(flags, test_c, source, white_box, scratch)
        tests = list_tests(binary)
        if not tests:
            sys.exit('the test binary lists no tests (does it start? check LD_LIBRARY_PATH)')
        base_fail = [t for t in tests if not run_test(binary, t, 60)]
        if base_fail:
            sys.exit(f'tests already failing on the unmodified source: {base_fail}')
        src_lines = source.read_text().split('\n')
        cands = []
        for ln in sorted(hit):
            for kind, new in mutants_for_line(src_lines[ln - 1]):
                if new != src_lines[ln - 1]:
                    cands.append((ln, kind, new))
        random.Random(a.seed).shuffle(cands)
        cands = cands[:a.n]
        print(f'{test_c.name}: {len(tests)} tests, {len(hit)} executed lines, {len(cands)} mutants to try', flush=True)

        def one(item):
            idx, (ln, kind, new) = item
            d = scratch / f'm{idx}'
            (d / 'Source').mkdir(parents=True)
            text = list(src_lines)
            text[ln - 1] = new
            msrc = d / 'Source' / source.name
            msrc.write_text('\n'.join(text))
            bin_ = d / 'bin'
            cmd = gcc_cmd(flags, test_c, None if white_box else msrc, bin_, scratch_inc=d if white_box else None)
            if not compile_ok(cmd):
                shutil.rmtree(d, ignore_errors=True)
                return None
            failed = [t for t in tests if not run_test(bin_, t, a.timeout)]
            shutil.rmtree(d, ignore_errors=True)
            return {'line': ln, 'kind': kind, 'was': src_lines[ln - 1].strip(), 'now': new.strip(), 'killed_by': failed}

        with ThreadPoolExecutor(a.j) as ex:
            results = [r for r in ex.map(one, enumerate(cands)) if r is not None]

        killed = [r for r in results if r['killed_by']]
        survivors = [r for r in results if not r['killed_by']]
        kills = {t: 0 for t in tests}
        sole = {t: 0 for t in tests}
        for r in killed:
            for t in r['killed_by']:
                kills[t] += 1
            if len(r['killed_by']) == 1:
                sole[r['killed_by'][0]] += 1
        data = {'test_file': str(test_c.relative_to(REPO)), 'source': str(source.relative_to(REPO)), 'tests': len(tests),
                'executed_lines': len(hit), 'valid_mutants': len(results), 'killed': len(killed),
                'score': round(100.0 * len(killed) / max(len(results), 1), 1),
                'tests_killing_nothing': [t for t in tests if kills[t] == 0],
                'kills_per_test': kills, 'sole_kills_per_test': sole, 'survivors': survivors}
        (out_dir / f'{name}__{source.stem}.json').write_text(json.dumps(data, indent=1))
        print(f'  mutation score {data["score"]}%  ({len(killed)}/{len(results)} valid mutants killed)')
        print(f'  tests that killed nothing ({len(data["tests_killing_nothing"])}/{len(tests)}):')
        for t in data['tests_killing_nothing']:
            print(f'    - {t}')
        print(f'  survivors ({len(survivors)}):')
        for r in survivors[:25]:
            print(f'    {source.name}:{r["line"]}  [{r["kind"]}]  {r["was"]}')
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == '__main__':
    main()
