#!/usr/bin/env python3
"""Run the pause/resume and renderer replays together.

Convenience entry point kept from the old monolithic replay; the checks themselves live in
pytest modules next to this file. They do not reproduce Android/JNI timing or measure
physical lipsync.
"""
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent

if __name__ == '__main__':
    raise SystemExit(pytest.main([
        '-v', '-c', str(HERE.parent / 'pytest.ini'),
        str(HERE / 'audio' / 'test_pause_resume_policy.py'),
        str(HERE / 'video' / 'test_slew_offset.py'),
        str(HERE / 'audio' / 'test_audiotrack_speed_epoch.py'),
        *sys.argv[1:],
    ]))
