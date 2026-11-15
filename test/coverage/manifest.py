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
