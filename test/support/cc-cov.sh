#!/bin/sh
# Compiler wrapper used to build the REAL sources with gcov instrumentation.
# Appended last so -O0 beats the Makefile's -O2: unoptimised code gives exact line coverage.
# Usage: make ... CC=<this script>   (see test/Makefile, target coverage-lib)
exec "${REAL_CC:-gcc}" "$@" -O0 --coverage -fprofile-update=atomic -ffunction-sections -fdata-sections -Wno-error
