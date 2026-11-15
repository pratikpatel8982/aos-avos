#!/usr/bin/env python3
"""Build the gcov-instrumented archive of the REAL sources that unit/integration tests link.

* sim objects  -> compiled by the repository's own Makefile (exact sim flags/defines), -O0 --coverage
* archive      -> libavos_cov.a = every sim object except main.o
* extra objects-> host-compilable sources the sim target does not link (audio_spdif, jni_sub_engine,
                  ...). Compiled instrumented so untested code shows as 0% instead of vanishing, but
                  NOT archived (several define conflicting symbols); tests #include or link them.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

from manifest import load

HERE = Path(__file__).resolve().parent
TEST = HERE.parent
REPO = TEST.parent


def make(repo, cov, ffmpeg, *args, capture=False):
    cmd = ['make', '-s', '--no-print-directory', '-C', str(repo), 'NO_DEPS=1', f'BUILD_DIR={cov}',
           f'TOOLCHAIN_PATH={ffmpeg}', f'CC={TEST / "support/cc-cov.sh"}', f'LINK={TEST / "support/cc-cov.sh"}', *args]
    return subprocess.run(cmd, check=True, text=True, capture_output=capture)


def sim_objects(repo, cov, ffmpeg):
    out = make(repo, cov, ffmpeg, '-f', 'Makefile', '-f', str(TEST / 'support/print-objs.mk'),
               'print-app-objs', capture=True).stdout.strip().splitlines()[-1]
    return sorted(Path(o).name for o in out.split() if o.endswith('.o'))


def capture_flags(repo, cov, ffmpeg):
    """Record the sim target's compile flags and link libraries, as the Makefile computes them."""
    wrapper = str(TEST / 'support/cc-cov.sh')
    compile_line = make(repo, cov, ffmpeg, 'VERBOSE=1', '-n', '-W', 'Source/util.c', str(cov / 'util.o'),
                        capture=True).stdout
    cmd = next(l for l in compile_line.splitlines() if l.startswith('echo ') and ' -c Source/util.c' in l)
    toks = shlex.split(cmd)[2:]                                  # drop "echo" and the compiler
    cflags, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t in ('-o', '-c'):
            i += 2
            continue
        if t not in ('-Werror', '-O2', '-g'):
            cflags.append(t)
        i += 1

    link_line = make(repo, cov, ffmpeg, 'VERBOSE=1', '-n', '-W', 'Source/main.c', str(cov / 'avos'),
                     capture=True).stdout
    link = next(l for l in link_line.splitlines() if f' -o {cov}/avos' in l and wrapper in l)
    ltoks = shlex.split(link)[1:]
    libs, i = [], 0
    while i < len(ltoks):
        t = ltoks[i]
        if t == '-o':
            i += 2
            continue
        if t.endswith('.o') or t.startswith('-L') or t == wrapper:
            i += 1
            continue
        if t.endswith('.a') and not os.path.isabs(t):          # ../audiocompress/x86_64/audiocompress.a
            t = str((repo / t).resolve())
        libs.append(t)
        i += 1
    if ffmpeg not in ('/', ''):
        libs = [f'-L{ffmpeg}/lib', f'-Wl,-rpath,{ffmpeg}/lib'] + libs
    (cov / 'flags.json').write_text(json.dumps(dict(cflags=cflags, libs=libs, repo=str(repo)), indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', type=Path, default=REPO)
    ap.add_argument('--out', type=Path, default=REPO / 'build-cov')
    ap.add_argument('--ffmpeg', default=os.environ.get('FFMPEG_PREFIX', '/'))
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    sim = sim_objects(args.repo, args.out, args.ffmpeg)
    excluded = {Path(p).stem for p in load()}
    extra = sorted(p.stem + '.o' for p in (args.repo / 'Source').glob('*.c')
                   if p.stem not in excluded and p.stem + '.o' not in sim)

    make(args.repo, args.out, args.ffmpeg, f'-j{args.jobs}', *[str(args.out / o) for o in sim + extra])

    # The instrumented executable (also builds ../audiocompress): used by integration tests that run avos.
    ldflags = [f'-L{args.ffmpeg}/lib', f'-Wl,-rpath,{args.ffmpeg}/lib', '-rdynamic', '-Wl,--gc-sections'] if args.ffmpeg not in ('/', '') else ['-rdynamic', '-Wl,--gc-sections']
    make(args.repo, args.out, args.ffmpeg, 'LDFLAGS=' + ' '.join(ldflags), str(args.out / 'avos'))
    capture_flags(args.repo, args.out, args.ffmpeg)

    archive = args.out / 'libavos_cov.a'
    archive.unlink(missing_ok=True)
    members = [str(args.out / o) for o in sim if o != 'main.o']
    subprocess.run(['ar', 'rcs', str(archive), *members], check=True)
    print(f'{archive}: {len(members)} members; {len(extra)} extra instrumented objects: '
          + ', '.join(o[:-2] for o in extra))


if __name__ == '__main__':
    sys.exit(main())
