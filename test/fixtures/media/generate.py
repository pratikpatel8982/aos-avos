#!/usr/bin/env python3
"""Regenerate the tiny deterministic media fixtures used by the unit/integration tests.

Everything is synthesised with ffmpeg's lavfi sources (no third-party media, no licensing issues),
2 seconds long, <= 160x120, so the whole directory stays small enough to commit. Needs the ffmpeg
CLI only to regenerate; the tests themselves just read the committed files.

    python3 test/fixtures/media/generate.py [--force]
"""
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent
FORCE = '--force' in sys.argv

VIDEO = ['-f', 'lavfi', '-i', 'testsrc=size=160x120:rate=10:duration=2']
AUDIO = ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=2']
STEREO = ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=2', '-ac', '2']
GLOBAL = ['-y', '-hide_banner', '-loglevel', 'error']
# Output options: strip metadata and encoder version strings so regeneration is reproducible.
REPRODUCIBLE = ['-map_metadata', '-1', '-fflags', '+bitexact', '-flags:v', '+bitexact', '-flags:a', '+bitexact']

# name -> ffmpeg arguments (inputs first, then codecs, output last)
FILES = {
    # ---- audio only
    'sine.wav': [*AUDIO, '-ar', '8000', '-t', '1', '-c:a', 'pcm_s16le'],
    'sine_stereo.wav': [*STEREO, '-ar', '8000', '-t', '1', '-c:a', 'pcm_s16le'],
    'sine.mp3': [*AUDIO, '-c:a', 'libmp3lame', '-b:a', '32k'],
    'sine_id3.mp3': [*AUDIO, '-c:a', 'libmp3lame', '-b:a', '32k', '-id3v2_version', '3',
                     '-metadata', 'title=Test Tone', '-metadata', 'artist=AVOS', '-metadata', 'album=Fixtures'],
    'sine.mp2': [*AUDIO, '-c:a', 'mp2', '-b:a', '64k'],
    'sine.flac': [*AUDIO, '-c:a', 'flac'],
    'sine.ogg': [*AUDIO, '-c:a', 'libvorbis', '-b:a', '32k'],
    'sine.opus': [*AUDIO, '-c:a', 'libopus', '-b:a', '24k', '-ar', '48000'],
    'sine.aac': [*AUDIO, '-c:a', 'aac', '-b:a', '32k'],
    'sine.m4a': [*AUDIO, '-c:a', 'aac', '-b:a', '32k'],
    'sine.ac3': [*STEREO, '-c:a', 'ac3', '-b:a', '96k'],
    'sine.dts': [*STEREO, '-c:a', 'dca', '-strict', '-2', '-b:a', '256k'],
    'sine.wma': [*AUDIO, '-c:a', 'wmav2', '-b:a', '32k'],
    # ---- video + audio, one per container/codec family the parser and decoders handle
    'h264_aac.mp4': [*VIDEO, *AUDIO, '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '5',
                     '-c:a', 'aac', '-b:a', '32k', '-movflags', '+faststart', '-shortest'],
    'h264_aac.mkv': [*VIDEO, *AUDIO, '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '5',
                     '-c:a', 'aac', '-b:a', '32k', '-shortest'],
    'h264_aac.ts': [*VIDEO, *AUDIO, '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '5',
                    '-c:a', 'aac', '-b:a', '32k', '-shortest'],
    'h264_aac.3gp': [*VIDEO, *AUDIO, '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
                     '-c:a', 'aac', '-b:a', '32k', '-ar', '44100', '-shortest'],
    'hevc_aac.mkv': [*VIDEO, *AUDIO, '-c:v', 'libx265', '-preset', 'ultrafast', '-x265-params', 'log-level=none',
                     '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '32k', '-shortest'],
    'mpeg4_mp3.avi': [*VIDEO, *AUDIO, '-c:v', 'mpeg4', '-q:v', '8', '-c:a', 'libmp3lame', '-b:a', '32k', '-shortest'],
    'mpeg2_mp2.mpg': [*VIDEO, *AUDIO, '-c:v', 'mpeg2video', '-q:v', '8', '-c:a', 'mp2', '-b:a', '64k', '-shortest'],
    'mpeg2_ac3.ts': [*VIDEO, *STEREO, '-c:v', 'mpeg2video', '-q:v', '8', '-c:a', 'ac3', '-b:a', '96k', '-shortest'],
    'vp8_vorbis.webm': [*VIDEO, *AUDIO, '-c:v', 'libvpx', '-b:v', '100k', '-c:a', 'libvorbis', '-b:a', '32k', '-shortest'],
    'vp9_opus.webm': [*VIDEO, *AUDIO, '-c:v', 'libvpx-vp9', '-b:v', '100k', '-c:a', 'libopus', '-b:a', '24k', '-ar', '48000', '-shortest'],
    'wmv2_wma.asf': [*VIDEO, *AUDIO, '-c:v', 'wmv2', '-q:v', '8', '-c:a', 'wmav2', '-b:a', '32k', '-shortest'],
    'flv_mp3.flv': [*VIDEO, *AUDIO, '-c:v', 'flv', '-q:v', '8', '-c:a', 'libmp3lame', '-b:a', '32k', '-ar', '44100', '-shortest'],
    'h263_aac.3gp': [*VIDEO, *AUDIO, '-s', '176x144', '-c:v', 'h263', '-c:a', 'aac', '-b:a', '32k', '-ar', '44100', '-shortest'],
    'rv10_ac3.rm': [*VIDEO, *STEREO, '-s', '176x144', '-c:v', 'rv10', '-c:a', 'ac3', '-b:a', '96k', '-shortest'],
    # ---- video only / stills
    'h264_only.mp4': [*VIDEO, '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '5', '-an'],
    'mjpeg.avi': [*VIDEO, '-c:v', 'mjpeg', '-q:v', '10', '-an'],
    'frame.png': ['-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=1:duration=1', '-frames:v', '1'],
    'frame.bmp': ['-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=1:duration=1', '-frames:v', '1', '-pix_fmt', 'bgr24'],
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
