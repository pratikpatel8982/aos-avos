"""Reads coverage/excluded.txt: the single source of truth for what is outside the percentage."""
from pathlib import Path

MANIFEST = Path(__file__).resolve().with_name('excluded.txt')


def load():
    """Return {repo-relative path: (category, reason)}."""
    out = {}
    for line in MANIFEST.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        path, category, reason = (part.strip() for part in line.split('|', 2))
        out[path] = (category, reason)
    return out
