#!/usr/bin/env python3
"""Coverage report for the AVOS native sources, built on gcovr.

Measures line and branch coverage of every host-compilable file in Source/, grouped by module.
Files listed in coverage/excluded.txt (i18n tables, Android-only code) are shown separately and
never counted. Files that were compiled but never executed count as 0%, so nothing hides.

    python3 test/coverage/report.py                      # table on stdout
    python3 test/coverage/report.py --html out/          # + browsable HTML
    python3 test/coverage/report.py --fail-under 90      # exit 1 below 90% line coverage
    python3 test/coverage/report.py --files              # per-file table, worst first
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from manifest import load

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

# Module = first matching prefix (longest first). Keeps the report aligned with how tests are filed.
MODULES = [
    ('subtitle', ('sub_', 'subtitle', 'stream_subtitle', 'stream_sub_ext', 'jni_sub', 'font_name', 'vobsub')),
    ('stream', ('stream', 'frame_q', 'pts_reorder')),
    ('audio', ('audio', 'downmix', 'pcm_', 'sonic', 'mixerdev', 'ac_av', 'dts', 'mp3', 'id3')),
    ('codec', ('codec', 'h264', 'hevc', 'mpeg2', 'mpg4', 'wmv', 'realvideo', 'cbe')),
    ('image/gfx', ('image', 'bmp', 'pixel', 'rect', 'fb', 'console', 'tty')),
    ('app/cli', ('app_', 'cli', 'main', 'platform', 'avsh', 'avos_', 'libavos', 'mainloop', 'device_config')),
    ('core/util', ()),
]


def module_of(path):
    name = Path(path).stem
    for module, prefixes in MODULES:
        if any(name.startswith(p) for p in prefixes):
            return module
    return 'core/util'


def run_gcovr(root, data_dirs, html):
    cmd = ['gcovr', '--root', str(root), '--filter', r'Source/.*\.c$', '--gcov-ignore-parse-errors=negative_hits.warn_once_per_file',
           '--gcov-ignore-parse-errors=suspicious_hits.warn_once_per_file',
           '--exclude-throw-branches', '--exclude-unreachable-branches',
           '--json', '-']
    for d in data_dirs:
        cmd += ['--object-directory', str(d)]
    if html:
        Path(html).mkdir(parents=True, exist_ok=True)
        cmd += ['--html-details', str(Path(html) / 'index.html')]
    done = subprocess.run(cmd, text=True, capture_output=True)
    if done.returncode:
        sys.exit('gcovr failed (exit %d):\n%s' % (done.returncode, done.stderr.strip()[-1500:]))
    out = done.stdout
    return json.loads(out)


def pct(covered, total):
    return 100.0 * covered / total if total else 100.0


def summarize(files):
    lines = sum(f['lines'] for f in files)
    hit = sum(f['hit'] for f in files)
    branches = sum(f['branches'] for f in files)
    bhit = sum(f['bhit'] for f in files)
    return lines, hit, branches, bhit


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', type=Path, default=REPO)
    ap.add_argument('--data', type=Path, action='append', help='directory with .gcno/.gcda (default: build-cov)')
    ap.add_argument('--html', help='write an HTML report to this directory')
    ap.add_argument('--json', dest='json_out', type=Path, help='write the per-file summary as JSON')
    ap.add_argument('--files', action='store_true', help='print the per-file table, worst first')
    ap.add_argument('--only', metavar='REGEX', help='restrict the per-file table to matching paths (implies --files)')
    ap.add_argument('--missing', action='store_true', help='list the uncovered line numbers of the files shown')
    ap.add_argument('--fail-under', type=float, help='exit 1 if line coverage is below this percentage')
    args = ap.parse_args()

    manifest = load()
    raw = run_gcovr(args.root, args.data or [args.root / 'build-cov'], args.html)

    files = []
    for entry in raw['files']:
        path = entry['file']
        if path in manifest:
            continue
        lines = [ln for ln in entry['lines'] if not ln.get('gcovr/noncode')]
        branches = [b for ln in lines for b in ln.get('branches', [])]
        files.append(dict(
            file=path, module=module_of(path),
            lines=len(lines), hit=sum(1 for ln in lines if ln['count'] > 0),
            branches=len(branches), bhit=sum(1 for b in branches if b['count'] > 0),
            missing=[ln['line_number'] for ln in lines if ln['count'] == 0]))

    lines, hit, branches, bhit = summarize(files)
    print(f'\nAVOS native coverage -- {len(files)} files, {lines} executable lines\n')
    print(f'  {"module":<12}{"files":>6}{"lines":>9}{"hit":>9}{"line %":>9}{"branches":>10}{"branch %":>10}')
    for module in [m for m, _ in MODULES]:
        sel = [f for f in files if f['module'] == module]
        if not sel:
            continue
        l, h, b, bh = summarize(sel)
        print(f'  {module:<12}{len(sel):>6}{l:>9}{h:>9}{pct(h, l):>8.1f}%{b:>10}{pct(bh, b):>9.1f}%')
    print(f'  {"TOTAL":<12}{len(files):>6}{lines:>9}{hit:>9}{pct(hit, lines):>8.1f}%{branches:>10}{pct(bhit, branches):>9.1f}%')

    if args.files or args.only or args.missing:
        shown = [f for f in files if not args.only or re.search(args.only, f['file'])]
        print(f'\n  {"file":<46}{"lines":>7}{"missed":>8}{"line %":>9}{"branch %":>10}')
        for f in sorted(shown, key=lambda f: (pct(f['hit'], f['lines']), -f['lines'])):
            print(f'  {f["file"]:<46}{f["lines"]:>7}{f["lines"] - f["hit"]:>8}{pct(f["hit"], f["lines"]):>8.1f}%{pct(f["bhit"], f["branches"]):>9.1f}%')
            if args.missing and f['missing']:
                runs, start = [], f['missing'][0]
                for a, b in zip(f['missing'], f['missing'][1:] + [None]):
                    if b != a + 1:
                        runs.append(str(start) if start == a else f'{start}-{a}')
                        start = b
                print('      uncovered: ' + ', '.join(runs))

    by_cat = {}
    for path, (cat, _reason) in manifest.items():
        by_cat.setdefault(cat, []).append(path)
    print('\n  Outside the percentage (coverage/excluded.txt):')
    for cat, paths in sorted(by_cat.items()):
        print(f'    {cat:<8}{len(paths):>3} files')

    if args.json_out:
        args.json_out.write_text(json.dumps(dict(
            total=dict(lines=lines, hit=hit, branches=branches, bhit=bhit), files=files), indent=1))

    if args.fail_under is not None and pct(hit, lines) < args.fail_under:
        print(f'\nFAIL: line coverage {pct(hit, lines):.1f}% < required {args.fail_under}%', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
