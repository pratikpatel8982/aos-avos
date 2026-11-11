#!/bin/sh
# Compiler wrapper for the whole-code audits (test/tools/audit_build.py): the repository's own Makefile
# builds every source, and the audit flags in $AVOS_AUDIT_FLAGS are appended last. -Wno-error keeps one
# warning from stopping the build, so every file is analysed.
exec "${REAL_CC:-gcc}" "$@" ${AVOS_AUDIT_FLAGS} -Wno-error
