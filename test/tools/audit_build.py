#!/usr/bin/env python3
"""Whole-code audit: run compiler diagnostics and static analysers over EVERY host-compilable source.

    python3 test/tools/audit_build.py warnings     # gcc -Wall -Wextra -Wformat=2 -Wshadow -Wnull-dereference ...
    python3 test/tools/audit_build.py analyzer     # gcc -fanalyzer: leaks, double free, use after free, NULL deref
    python3 test/tools/audit_build.py tidy         # clang-tidy clang-analyzer-*, bugprone-*, concurrency-*

The sources are built by the repository's own Makefile (exact sim flags and defines) with a compiler wrapper,
so nothing in Source/ or the Makefile is changed. Output: build-audit/<mode>.log (raw) and
build-audit/<mode>.summary.txt (deduplicated findings by check and by file). These are findings to triage,
not test failures: static analysers have false positives, so read each one before acting on it.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEST = HERE.parent
REPO = TEST.parent
sys.path.insert(0, str(TEST / 'coverage'))
from build_lib import sim_objects  # noqa: E402
from manifest import load  # noqa: E402

WARN_FLAGS = ('-Wall -Wextra -Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2 -Wshadow -Wnull-dereference '
              '-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wcast-align -Wundef -Wstrict-overflow=2 '
              '-Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers')
ANALYZER_FLAGS = '-fanalyzer -Wno-analyzer-malloc-leak-off-by-default'
TIDY_CHECKS = ('-*,clang-analyzer-*,bugprone-*,concurrency-*,-bugprone-easily-swappable-parameters,'
               '-bugprone-reserved-identifier,-bugprone-narrowing-conversions,-bugprone-implicit-widening-of-multiplication-result,'
               '-clang-analyzer-optin.performance.Padding,-clang-analyzer-security.insecureAPI.*')
DIAG = re.compile(r'^(?P<file>[^\s:]+):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning|error): (?P<msg>.*?)(?: \[(?P<check>[^\]]+)\])?$')


def targets(ffmpeg):
    cov = REPO / 'build-cov'
    sim = sim_objects(REPO, cov, ffmpeg)
    excluded = {Path(p).stem for p in load()}
    extra = sorted(p.stem + '.o' for p in (REPO / 'Source').glob('*.c') if p.stem not in excluded and p.stem + '.o' not in sim)
    return sim, extra, sorted(Path(REPO / 'Source' / (o[:-2] + '.c')) for o in sim + extra if o != 'main.o')


def run_compiler_audit(mode, flags, ffmpeg, jobs):
    out = REPO / 'build-audit'
    bdir = out / mode
    bdir.mkdir(parents=True, exist_ok=True)
    for stale in bdir.glob('*.o'):          # make would skip up-to-date objects and report nothing
        stale.unlink()
    sim, extra, _ = targets(ffmpeg)
    wrapper = str(TEST / 'support/cc-audit.sh')
    cmd = ['make', '-k', '-s', '--no-print-directory', f'-j{jobs}', '-C', str(REPO), 'NO_DEPS=1', f'BUILD_DIR={bdir}',
           f'TOOLCHAIN_PATH={ffmpeg}', f'CC={wrapper}', f'LINK={wrapper}', *[str(bdir / o) for o in sim + extra]]
    done = subprocess.run(cmd, text=True, capture_output=True, env=dict(os.environ, AVOS_AUDIT_FLAGS=flags))
    return done.stderr + done.stdout


def run_tidy(ffmpeg, jobs):
    flags = json.loads((REPO / 'build-cov' / 'flags.json').read_text())['cflags']
    _, _, files = targets(ffmpeg)

    def one(src):
        done = subprocess.run(['clang-tidy', f'-checks={TIDY_CHECKS}', '-quiet', str(src), '--',
                               '-std=gnu17', *flags], cwd=REPO, text=True, capture_output=True)
        return done.stdout + done.stderr

    with ThreadPoolExecutor(jobs) as ex:
        return ''.join(ex.map(one, files))


def summarise(mode, log):
    seen, by_check, by_file = set(), collections.Counter(), collections.Counter()
    items = []
    for raw in log.splitlines():
        m = DIAG.match(raw.strip())
        if not m:
            continue
        path = Path(m['file'])
        path = path if path.is_absolute() else REPO / path
        try:
            rel = str(path.resolve().relative_to(REPO))
        except ValueError:
            continue                                      # system / FFmpeg headers
        if rel.startswith('external/'):
            continue
        key = (rel, m['line'], m['col'], m['check'] or m['msg'])
        if key in seen:
            continue
        seen.add(key)
        check = m['check'] or ('error' if m['sev'] == 'error' else 'warning')
        by_check[check] += 1
        by_file[rel] += 1
        items.append((rel, int(m['line']), check, m['msg']))
    lines = [f'{mode}: {len(items)} distinct findings in {len(by_file)} files', '', 'by check:']
    lines += [f'  {n:5d}  {c}' for c, n in by_check.most_common()]
    lines += ['', 'by file (top 25):'] + [f'  {n:5d}  {f}' for f, n in by_file.most_common(25)]
    lines += ['', 'findings (file:line  check  message):']
    lines += [f'  {f}:{l}  {c}  {msg}' for f, l, c, msg in sorted(items)]
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['warnings', 'analyzer', 'tidy'])
    ap.add_argument('--ffmpeg', default=os.environ.get('FFMPEG_PREFIX', '/'))
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 2)
    a = ap.parse_args()
    if not (REPO / 'build-cov' / 'flags.json').exists():
        sys.exit('build-cov/flags.json missing: run  make -C test coverage-lib  first')
    out = REPO / 'build-audit'
    out.mkdir(exist_ok=True)
    if a.mode == 'tidy':
        log = run_tidy(a.ffmpeg, a.jobs)
    else:
        log = run_compiler_audit(a.mode, WARN_FLAGS if a.mode == 'warnings' else ANALYZER_FLAGS, a.ffmpeg, a.jobs)
    (out / f'{a.mode}.log').write_text(log)
    summary = summarise(a.mode, log)
    (out / f'{a.mode}.summary.txt').write_text(summary)
    print('\n'.join(summary.splitlines()[:45]))
    print(f'\nfull list: {out / (a.mode + ".summary.txt")}')


if __name__ == '__main__':
    main()
