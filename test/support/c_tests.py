"""pytest plugin: every ``TEST()`` in a ``test_*.c`` file under unit/ or integration/ is a test item.

Each C test file is ONE translation unit built against the real, gcov-instrumented sources:

    cc <sim cflags> --coverage -O0 -Isupport -I<repo> test_x.c  libavos_cov.a  <sim libs>

The flags and libs come from build-cov/flags.json, written by coverage/build_lib.py from the
repository's own Makefile, so a unit test is compiled exactly like production. The library is
built on first use (needs FFMPEG_PREFIX, or an FFmpeg >= 7.1 on the default search path).
"""
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

from paths import FIXTURES_DIR, INCLUDE_DIR, REPO_ROOT, TEST_DIR

COV_DIR = Path(os.environ.get('COV_DIR', REPO_ROOT / 'build-cov'))
TEST_RE = re.compile(r'^\s*TEST\(\s*([A-Za-z_]\w*)\s*\)', re.M)
_built = {}          # test .c path -> (binary, error)
_flags = None


def _ensure_library():
    global _flags
    if _flags is not None:
        return _flags
    flags = COV_DIR / 'flags.json'
    if not (COV_DIR / 'libavos_cov.a').exists() or not flags.exists():
        cmd = [sys.executable, str(TEST_DIR / 'coverage' / 'build_lib.py'), '--out', str(COV_DIR)]
        if os.environ.get('FFMPEG_PREFIX'):
            cmd += ['--ffmpeg', os.environ['FFMPEG_PREFIX']]
        subprocess.run(cmd, check=True, capture_output=True, text=True, cwd=REPO_ROOT)
    _flags = json.loads(flags.read_text())
    return _flags


def build(source):
    """Compile+link one test file; returns (binary_path, error_text_or_None). Cached per session."""
    source = Path(source)
    if source in _built:
        return _built[source]
    flags = _ensure_library()
    rel = source.resolve().relative_to(TEST_DIR)
    out = COV_DIR / 'tests' / rel.with_suffix('')
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [os.environ.get('REAL_CC', 'gcc'), *flags['cflags'],
           '-O0', '-g', '--coverage', '-fprofile-update=atomic', '-ffunction-sections', '-fdata-sections',
           '-Wno-error', '-I' + str(TEST_DIR / 'support'), '-I' + str(REPO_ROOT),
           f'-DAVOS_TEST_FIXTURES="{FIXTURES_DIR}"',
           str(source), str(COV_DIR / 'libavos_cov.a'), *flags['libs'], '-Wl,--gc-sections', '-o', str(out)]
    done = subprocess.run(cmd, cwd=REPO_ROOT, text=True, capture_output=True)
    result = (out, None) if done.returncode == 0 else (None, done.stderr[-6000:] or done.stdout[-6000:])
    _built[source] = result
    return result


class CFile(pytest.File):
    def collect(self):
        for name in TEST_RE.findall(self.path.read_text()):
            yield CTest.from_parent(self, name=name)


class CTest(pytest.Item):
    def runtest(self):
        binary, error = build(self.path)
        if error:
            raise CBuildError(error)
        env = dict(os.environ, GCOV_ERROR_FILE='/dev/null')
        done = subprocess.run([str(binary), self.name], cwd=REPO_ROOT, env=env, text=True,
                              capture_output=True, timeout=int(os.environ.get('C_TEST_TIMEOUT', '120')))
        if done.returncode != 0:
            raise CTestFailure(f'exit status {done.returncode}\n{done.stdout}{done.stderr}'.rstrip())

    def repr_failure(self, excinfo, style=None):
        if isinstance(excinfo.value, (CBuildError, CTestFailure)):
            kind = 'build failed' if isinstance(excinfo.value, CBuildError) else 'test failed'
            return f'{kind}: {self.path.relative_to(TEST_DIR)}::{self.name}\n{excinfo.value}'
        return super().repr_failure(excinfo, style)

    def reportinfo(self):
        return self.path, 0, f'{self.path.relative_to(TEST_DIR)}::{self.name}'


class CBuildError(Exception):
    pass


class CTestFailure(Exception):
    pass


def pytest_collect_file(file_path, parent):
    if file_path.suffix == '.c' and file_path.name.startswith('test_'):
        parts = file_path.parts
        if 'unit' in parts or 'integration' in parts:
            return CFile.from_parent(parent, path=file_path)
