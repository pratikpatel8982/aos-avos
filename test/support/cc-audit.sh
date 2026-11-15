#!/bin/sh
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

# Compiler wrapper for the whole-code audits (test/tools/audit_build.py): the repository's own Makefile
# builds every source, and the audit flags in $AVOS_AUDIT_FLAGS are appended last. -Wno-error keeps one
# warning from stopping the build, so every file is analysed.
exec "${REAL_CC:-gcc}" "$@" ${AVOS_AUDIT_FLAGS} -Wno-error
