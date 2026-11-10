#!/bin/sh
# Compiler wrapper used to build the REAL sources with gcov instrumentation.
# Appended last so -O0 beats the Makefile's -O2: unoptimised code gives exact line coverage.
# Usage: make ... CC=<this script>   (see test/Makefile, target coverage-lib)
# AVOS_SAN=address,undefined (or thread) additionally builds with that sanitizer; the test files are built
# with the same flag (support/c_tests.py), so the whole unit suite runs under it. See `make -C test sanitize-*`.
SAN=""
[ -n "$AVOS_SAN" ] && SAN="-fsanitize=$AVOS_SAN -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
exec "${REAL_CC:-gcc}" "$@" -O0 --coverage -fprofile-update=atomic -ffunction-sections -fdata-sections -Wno-error $SAN
