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

# Compiler wrapper used to build the REAL sources with gcov instrumentation.
# Appended last so -O0 beats the Makefile's -O2: unoptimised code gives exact line coverage.
# Usage: make ... CC=<this script>   (see test/Makefile, target coverage-lib)
# AVOS_SAN=address,undefined (or thread) additionally builds with that sanitizer; the test files are built
# with the same flag (support/c_tests.py), so the whole unit suite runs under it. See `make -C test sanitize-*`.
SAN=""
[ -n "$AVOS_SAN" ] && SAN="-fsanitize=$AVOS_SAN -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
# AVOS_SAN_NO=shift-base: checks to leave out (the left-shift-into-sign-bit report from Include/get.h is known and
# would abort every parser test at the first byte-assembly, hiding everything behind it)
[ -n "$AVOS_SAN" ] && [ -n "$AVOS_SAN_NO" ] && SAN="$SAN -fno-sanitize=$AVOS_SAN_NO"
exec "${REAL_CC:-gcc}" "$@" -O0 --coverage -fprofile-update=atomic -ffunction-sections -fdata-sections -Wno-error $SAN
