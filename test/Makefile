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
.PHONY: all help check unit replay integration tooling subtitles coverage-lib coverage coverage-report coverage-clean legacy clean \
        audit-warnings audit-analyzer audit-tidy sanitize-asan sanitize-tsan mutate
all: check

help:
	@echo "check            run every automated test (unit, replay, integration, tooling)"
	@echo "unit|replay|integration|tooling   one test type"
	@echo "subtitles        native subtitle host harness (sanitizers via SANITIZERS=)"
	@echo "coverage         run everything instrumented and print the gcov summary"
	@echo "coverage-report  re-render the report from existing .gcda data"
	@echo "legacy           build the manual utilities in tools/legacy"
	@echo ""
	@echo "bug hunting (findings to triage; see README 'Finding bugs'):"
	@echo "audit-warnings   whole code, gcc -Wall -Wextra -Wformat=2 -Wshadow -Wnull-dereference ..."
	@echo "audit-analyzer   whole code, gcc -fanalyzer (leaks, double free, use after free, NULL deref)"
	@echo "audit-tidy       whole code, clang-tidy clang-analyzer/bugprone/concurrency checks"
	@echo "sanitize-asan    whole unit+integration suite under AddressSanitizer + UBSan (separate build-asan/)"
	@echo "sanitize-tsan    unit+integration suite under ThreadSanitizer (separate build-tsan/)"
	@echo "mutate           mutation audit: does every unit test fail when the code is broken? (slow)"

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
	rm -rf $(COV_DIR) $(REPO)/build-asan $(REPO)/build-tsan $(REPO)/build-audit __pycache__ .pytest_cache

# ---- bug hunting --------------------------------------------------------------------------
# Reports go to build-audit/. They are findings to read, not pass/fail gates.
audit-warnings audit-analyzer audit-tidy: $(COV_LIB)
	python3 $(HERE)/tools/audit_build.py $(subst audit-,,$@) --ffmpeg $(FFMPEG_PREFIX) -j $(JOBS)

# The same real sources and the same tests, built with a sanitizer; any report fails the test.
# AddressSanitizer: out-of-bounds, use-after-free, leaks.  UBSan: signed overflow, bad shifts, misaligned access.
# ThreadSanitizer: data races (only meaningful where a test actually runs threads).
# known_defect_* tests are left out: they pin real bugs by making the code misbehave (e.g. read past a buffer), which a
# sanitizer rightly reports; the bugs are listed in the README and covered in the normal build by guard-page tests.
# Shift-base UB (Include/get.h, sonic.c) is a known finding and is compiled out of the run so it does not hide the rest.
SAN_ENV_asan = AVOS_SAN=address,undefined AVOS_SAN_NO=shift-base ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1
SAN_ENV_tsan = AVOS_SAN=thread TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:allocator_may_return_null=1
sanitize-asan sanitize-tsan:
	$(SAN_ENV_$(subst sanitize-,,$@)) COV_DIR=$(REPO)/build-$(subst sanitize-,,$@) \
	    python3 $(HERE)/coverage/build_lib.py --repo $(REPO) --out $(REPO)/build-$(subst sanitize-,,$@) --ffmpeg $(FFMPEG_PREFIX) -j $(JOBS)
	$(SAN_ENV_$(subst sanitize-,,$@)) COV_DIR=$(REPO)/build-$(subst sanitize-,,$@) \
	    $(PYTEST) $(HERE) -m "unit or integration" --ignore=$(HERE)/integration/subtitles -q -k "not known_defect"

JOBS_MUT ?= 2
MUTANTS  ?= 100
mutate: $(COV_LIB)
	@mkdir -p $(REPO)/build-cov/mutation
	@for t in $$(find $(HERE)/unit -name 'test_*.c' | sort); do \
	    case $$t in \
	      *test_stream_sinks.c) ;; \
	      *) python3 $(HERE)/tools/mutate.py $$t -n $(MUTANTS) -j $(JOBS_MUT) || echo "FAILED: $$t";; \
	    esac; echo; done
