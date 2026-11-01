# AVOS test suite

Everything here exercises the **real** sources in `Source/`. Production code is never replaced by
a fake; the only stand-ins are for things that do not exist on a Linux/macOS host (Android).

## Layout: by type, then by module

```
test/
├── unit/            C tests that link the real, gcov-instrumented sources (one TEST() = one pytest item)
│   ├── core/        object, linked_list, util, file, i18n, timers, threads, iso tables, ...
│   ├── audio/       sonic, downmix, id3, pcm_autogain, audio_interface_*, spdif, mp3, ...
│   ├── codec/       h264/hevc/mpeg2/mpg4/wmv parsers, codec_utils, codec_ffmpeg_*, ...
│   ├── gfx/         image, bmp, pixel_utils, rect, fb, console, tty
│   ├── stream/      stream*, frame_q, pts_reorder, buffers, io, parser, sync, sinks, filters
│   ├── subtitle/    subtitle_*, sub_format_*, font_name_parser, vobsub
│   └── app/         cli, avsh, libavos, mainloop, app_start/stop, device_config
├── integration/     several real modules together
│   ├── subtitles/   native host harness (real engine + libass, sanitizers, race tests)
│   ├── ffmpeg/      colour conversion and seek-cancel against real FFmpeg
│   └── media/       real files from fixtures/media played/probed through the real stack
├── replay/          regression tests that recompile functions *extracted* from Android-only files
│   ├── audio/  sync/  video/  android/        (cannot be gcov-measured; see below)
├── tooling/         tests of the device-side analyzers and campaign scripts (pure Python)
├── device/          adb harnesses + log analyzers (run on a phone, not in CI)
├── support/         shared helpers: avos_test.h, c_tests.py (pytest plugin), paths.py, native_replay.py
├── coverage/        gcov build + report tooling, excluded.txt (what is outside the percentage, and why)
├── fixtures/        media/ (generated, tiny), subtitles/ (hand-written), replay data
└── tools/legacy/    old manual utilities (comp, ff, stream_url)
```

A file belongs where its **subject** lives. A test that needs two modules to be meaningful goes
in `integration/`. A test that needs a phone goes in `device/`.

## Running

```bash
export FFMPEG_PREFIX=/path/to/ffmpeg-9      # FFmpeg >= 7.1 (CI uses conda-forge ffmpeg 9)

make -C test check            # everything automated
make -C test unit             # or: replay | integration | tooling
make -C test subtitles        # native subtitle harness (SANITIZERS=thread|address|undefined)
make -C test coverage         # run everything instrumented, print the gcov report

pytest test/unit/core                      # one directory
pytest test/unit/core/test_linked_list.c   # one C file
pytest -k 'insert and not remove' test/unit
```

`make test` in the repository root runs `make -C test`.

## Writing a C unit test

A test file is one translation unit named `test_<module>.c` under `unit/<area>/`:

```c
#include "avos_test.h"
#include "linked_list.h"          /* public API: links the real libavos_cov.a */

TEST(append_keeps_order) {
    ...
    CHECK_EQ_INT(LinkedList_count(&l), 3);
}
```

* **Public API only:** include the module's header; the real instrumented code is linked in.
* **White box:** `#include "Source/foo.c"` compiles the real file *into* the test, so `static`
  functions and file-local state are reachable, and gcov attributes the lines to `Source/foo.c`.
* **Never stub production code.** If something is hard to reach, build a real input for it
  (a real file from `fixtures/`, a real socket pair, a real thread) rather than faking the callee.
* Every `TEST()` runs in its own forked process: a crash fails only that test and no state leaks.
* `CHECK_*` records a failure and continues; `REQUIRE_*` aborts the test.
* `CHECK_ABORTS(stmt)` asserts that `stmt` hits an `assert()`/`abort()` (contract violations are
  code too, and are counted); `CHECK_SURVIVES(stmt)` is its converse. Both run in a child process
  and flush gcov counters before it dies.
* `fixture_path("media/h264_aac.mp4")` gives the absolute path of a fixture.
* Commas inside a macro argument (e.g. a compound literal `(int[]){1,2}`) split it; name the array.
* Assertions must be able to fail. Do not write `x == y || 1`, or checks of values the code just
  assigned from the same expression. Assert **observable behaviour**: return values, resulting
  state, bytes produced, ordering, error codes.

Tests are compiled with the sim target's own flags (`build-cov/flags.json`, produced from the
repository Makefile), so a unit test is built the way production is.

## Coverage

`make -C test coverage-lib` builds `build-cov/libavos_cov.a`: every sim object compiled by the
repo's own Makefile at `-O0 --coverage`, plus an instrumented `build-cov/avos` executable.
Seven host-compilable files the sim does not link are instrumented too (so untested code shows as
0%, never as absent).

```bash
python3 test/coverage/report.py                         # per-module table
python3 test/coverage/report.py --only 'stream_buffer'  # per-file, with --missing for line numbers
python3 test/coverage/report.py --html out/             # browsable
python3 test/coverage/report.py --fail-under 90         # CI gate
```

### What is, and is not, in the percentage

`coverage/excluded.txt` is the single source of truth.

* **i18n** (4 files): code-page lookup tables, excluded by project decision.
* **android** (20 files): do not compile on a host at all (JNI/NDK/OpenSL/ALSA/AudioTrack,
  `avos_mp*`, `codec_sfdec2`, the Android video sinks). They are verified by `replay/`, which
  extracts production functions and recompiles them, which proves behaviour but cannot be
  gcov-measured, so they are reported separately rather than counted as covered.

Everything else (125 files, ~31.7k executable lines) is measured. Branch coverage is reported
beside line coverage; `assert()` failure paths and `-Wunused` dead code are the usual leftovers.
Code that is genuinely unreachable on a host (platform `#ifdef`s, error paths needing a failing
`malloc` or a vanished kernel facility) is documented next to the test file as a short
`/* UNREACHABLE: ... */` list rather than being hidden or faked.

## Environment

| Variable | Meaning |
|---|---|
| `FFMPEG_PREFIX` | FFmpeg >= 7.1 install prefix (pkg-config + loader paths are derived from it) |
| `CC` / `REAL_CC` | compiler for replay tests / for unit tests (default `cc` / `gcc`) |
| `COV_DIR` | coverage build directory (default `<repo>/build-cov`) |
| `COVERAGE=1` | make the subtitle harness keep gcov data under `COV_DIR` |
| `SANITIZERS` | subtitle harness: `undefined` (default), `thread`, `address`, `none` |
| `C_TEST_TIMEOUT` | seconds per C test (default 120) |
