#!/usr/bin/env python3
# Copyright 2026 Courville Software
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Regenerate the tiny deterministic media fixtures used by the unit/integration tests.

Everything is synthesised with ffmpeg's lavfi sources (no third-party media, no licensing issues),
2 seconds long, so the whole directory stays small enough to commit. Needs the ffmpeg
CLI only to regenerate; the tests themselves just read the committed files.

    python3 test/fixtures/media/generate.py [--force]
"""
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent
FORCE = '--force' in sys.argv

AUDIO = ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=2']
STEREO = ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=2', '-ac', '2']
GLOBAL = ['-y', '-hide_banner', '-loglevel', 'error']
# Output options: strip metadata and encoder version strings so regeneration is reproducible.
REPRODUCIBLE = ['-map_metadata', '-1', '-fflags', '+bitexact', '-flags:v', '+bitexact', '-flags:a', '+bitexact']

# name -> ffmpeg arguments (inputs first, then codecs, output last)
FILES = {
    'sine.wav': [*AUDIO, '-ar', '8000', '-t', '1', '-c:a', 'pcm_s16le'],
    'sine.mp3': [*AUDIO, '-c:a', 'libmp3lame', '-b:a', '32k'],
    'sine_id3.mp3': [*AUDIO, '-c:a', 'libmp3lame', '-b:a', '32k', '-id3v2_version', '3',
                     '-metadata', 'title=Test Tone', '-metadata', 'artist=AVOS', '-metadata', 'album=Fixtures'],
    'sine.ac3': [*STEREO, '-c:a', 'ac3', '-b:a', '96k'],
    'frame.png': ['-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=1:duration=1', '-frames:v', '1'],
    'frame.jpg': ['-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=1:duration=1', '-frames:v', '1', '-q:v', '5'],
}


def main():
    made = skipped = 0
    for name, args in FILES.items():
        target = OUT / name
        if target.exists() and not FORCE:
            skipped += 1
            continue
        cmd = ['ffmpeg', *GLOBAL, *args, *REPRODUCIBLE, str(target)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode:
            print(f'FAILED {name}: {done.stderr.strip().splitlines()[-1] if done.stderr else done.returncode}')
            target.unlink(missing_ok=True)
            continue
        made += 1
    total = sum(p.stat().st_size for p in OUT.iterdir() if p.is_file() and p.name != 'generate.py')
    print(f'{made} generated, {skipped} kept; {len(list(OUT.glob("*"))) - 1} files, {total / 1024:.0f} KiB total')


if __name__ == '__main__':
    main()
