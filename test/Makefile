# AVOS test suite entry point.   make -C test help
#
#   FFMPEG_PREFIX   install prefix of FFmpeg >= 7.1 (CI uses conda-forge ffmpeg 9)
#   CC              compiler for the replay/integration tests (default cc)
#   JOBS            parallel jobs for building the instrumented library
#
# Layout (see README.md):  unit/  replay/  integration/  tooling/  device/  support/  tools/

REPO      := $(abspath ..)
HERE      := $(abspath .)
FFMPEG_PREFIX ?= /
JOBS      ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
COV_DIR   ?= $(REPO)/build-cov
COV_LIB   := $(COV_DIR)/libavos_cov.a
PYTEST    ?= python3 -m pytest
export FFMPEG_PREFIX

# `make test` in the repo root runs this.
.PHONY: all help check unit replay integration tooling subtitles coverage-lib coverage coverage-report coverage-clean legacy clean
all: check

help:
	@echo "check            run every automated test (unit, replay, integration, tooling)"
	@echo "unit|replay|integration|tooling   one test type"
	@echo "subtitles        native subtitle host harness (sanitizers via SANITIZERS=)"
	@echo "coverage         run everything instrumented and print the gcov summary"
	@echo "coverage-report  re-render the report from existing .gcda data"
	@echo "legacy           build the manual utilities in tools/legacy"

check:
	$(PYTEST) $(HERE) --ignore=$(HERE)/integration/subtitles
	$(MAKE) subtitles

unit replay integration tooling:
	$(PYTEST) $(HERE) -m $@ --ignore=$(HERE)/integration/subtitles

subtitles:
	python3 $(HERE)/integration/subtitles/test_host_native.py

# ---- coverage ---------------------------------------------------------------------------
# The REAL sources are compiled once with gcov at -O0 using the repository's own Makefile (so
# flags, defines and include paths are exactly the sim target's), archived without main.o, and
# every unit/integration test links that archive.  No stubs of production code.
# What is outside the percentage, and why, is listed in coverage/excluded.txt.
coverage-lib: $(COV_LIB)

$(COV_LIB): $(HERE)/support/cc-cov.sh $(HERE)/coverage/build_lib.py
	python3 $(HERE)/coverage/build_lib.py --repo $(REPO) --out $(COV_DIR) --ffmpeg $(FFMPEG_PREFIX) -j $(JOBS)

coverage: coverage-clean coverage-lib
	COVERAGE=1 COV_DIR=$(COV_DIR) $(PYTEST) $(HERE) --ignore=$(HERE)/integration/subtitles -q
	COVERAGE=1 COV_DIR=$(COV_DIR) python3 $(HERE)/integration/subtitles/test_host_native.py
	$(MAKE) coverage-report

coverage-report:
	python3 $(HERE)/coverage/report.py --root $(REPO) --data $(COV_DIR)

coverage-clean:
	find $(COV_DIR) -name '*.gcda' -delete 2>/dev/null || true

legacy:
	$(MAKE) -C tools/legacy

clean:
	$(MAKE) -C tools/legacy clean
	rm -rf $(COV_DIR) __pycache__ .pytest_cache
