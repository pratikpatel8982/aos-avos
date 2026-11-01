/*
 * avos_test.h -- minimal, dependency-free unit-test header for the AVOS C sources.
 *
 * A test file is ONE translation unit that #includes this header, defines tests with TEST(),
 * and gets a main() for free:
 *
 *     #include "avos_test.h"
 *     #include "Source/linked_list.c"     // white-box: also reaches static functions
 *
 *     TEST(list_append_keeps_order) {
 *         ...
 *         CHECK_EQ_INT(list.count, 3);
 *     }
 *
 * `#include "Source/x.c"` compiles the REAL file into the test, so statics are reachable and gcov
 * attributes the lines to Source/x.c. Without it the test links the real, instrumented
 * libavos_cov.a and can only call the module's public API. Never replace production code with a
 * fake: link the real thing and drive it.
 *
 * Every test runs in its own forked process (a crash or abort fails only that test; state never
 * leaks between tests). Run one test with `./binary <name>`, list with `./binary --list`.
 * The pytest plugin (support/c_tests.py) turns each TEST() into a pytest item.
 *
 * CHECK_* record a failure and keep going; REQUIRE_* abort the current test.
 */
#ifndef AVOS_TEST_H
#define AVOS_TEST_H

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AVOS_TEST_FIXTURES
#define AVOS_TEST_FIXTURES "test/fixtures"
#endif

typedef void (*avos_test_fn)(void);
typedef struct { const char *name; avos_test_fn fn; } avos_test_case;

#define AVOS_TEST_MAX 4096
static avos_test_case avos_tests[AVOS_TEST_MAX];
static int avos_test_count;
static int avos_checks, avos_failures;
static jmp_buf avos_abort;
static const char *avos_current = "";

static void avos_register(const char *name, avos_test_fn fn)
{
    if (avos_test_count < AVOS_TEST_MAX)
        avos_tests[avos_test_count++] = (avos_test_case){ name, fn };
}

#define TEST(name)                                                              \
    static void name(void);                                                     \
    __attribute__((constructor)) static void avos_reg_##name(void) { avos_register(#name, name); } \
    static void name(void)

static void avos_fail(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void avos_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    avos_failures++;
    fprintf(stderr, "  FAIL %s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define CHECK(cond) do { avos_checks++; if (!(cond)) avos_fail(__FILE__, __LINE__, "CHECK(%s)", #cond); } while (0)
#define CHECK_MSG(cond, ...) do { avos_checks++; if (!(cond)) avos_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)
#define CHECK_EQ_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); avos_checks++; \
    if (_a != _b) avos_fail(__FILE__, __LINE__, "%s == %s  (got %lld, want %lld)", #a, #b, _a, _b); } while (0)
#define CHECK_EQ_UINT(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); avos_checks++; \
    if (_a != _b) avos_fail(__FILE__, __LINE__, "%s == %s  (got 0x%llx, want 0x%llx)", #a, #b, _a, _b); } while (0)
#define CHECK_EQ_STR(a, b) do { const char *_a = (a), *_b = (b); avos_checks++; \
    if (!_a || !_b ? _a != _b : strcmp(_a, _b)) avos_fail(__FILE__, __LINE__, "%s == %s  (got \"%s\", want \"%s\")", #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); } while (0)
#define CHECK_EQ_MEM(a, b, n) do { avos_checks++; if (memcmp((a), (b), (n))) avos_fail(__FILE__, __LINE__, "memcmp(%s, %s, %s) differs", #a, #b, #n); } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); avos_checks++; \
    if (fabs(_a - _b) > (tol)) avos_fail(__FILE__, __LINE__, "%s ~= %s  (got %g, want %g +-%g)", #a, #b, _a, _b, (double)(tol)); } while (0)
#define CHECK_NULL(p) do { avos_checks++; if ((p) != NULL) avos_fail(__FILE__, __LINE__, "%s is not NULL", #p); } while (0)
#define CHECK_NOT_NULL(p) do { avos_checks++; if ((p) == NULL) avos_fail(__FILE__, __LINE__, "%s is NULL", #p); } while (0)
#define REQUIRE(cond) do { avos_checks++; if (!(cond)) { avos_fail(__FILE__, __LINE__, "REQUIRE(%s)", #cond); longjmp(avos_abort, 1); } } while (0)
#define REQUIRE_NOT_NULL(p) do { avos_checks++; if ((p) == NULL) { avos_fail(__FILE__, __LINE__, "%s is NULL", #p); longjmp(avos_abort, 1); } } while (0)

/*
 * CHECK_ABORTS(stmt): `stmt` must terminate the process through assert()/abort() or a fatal signal.
 * It runs in a forked child, so the contract-violation path of production code (its assert()s) can be
 * exercised without taking the test down. The child flushes its gcov counters from the signal
 * handler first, otherwise the lines executed right before the abort would never be counted.
 * CHECK_SURVIVES(stmt) is the converse: the child must exit normally.
 */
extern void __gcov_dump(void);       /* libgcov: tests are always built with --coverage */
static void avos_die_handler(int sig)
{
    __gcov_dump();
    _exit(128 + sig);
}
static int avos_run_in_child(void (*body)(void *), void *arg)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (!freopen("/dev/null", "w", stderr)) _exit(120);
        signal(SIGABRT, avos_die_handler);
        signal(SIGSEGV, avos_die_handler);
        signal(SIGBUS, avos_die_handler);
        signal(SIGFPE, avos_die_handler);
        body(arg);
        __gcov_dump();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
#define CHECK_ABORTS(stmt) do { avos_checks++;                                                 \
    int _rc;                                                                                    \
    { void _b(void *_u) { (void)_u; stmt; } _rc = avos_run_in_child(_b, NULL); }                \
    if (_rc != 128 + SIGABRT) avos_fail(__FILE__, __LINE__, "expected abort from `%s`, child status %d", #stmt, _rc); } while (0)
#define CHECK_SURVIVES(stmt) do { avos_checks++;                                               \
    int _rc;                                                                                    \
    { void _b(void *_u) { (void)_u; stmt; } _rc = avos_run_in_child(_b, NULL); }                \
    if (_rc != 0) avos_fail(__FILE__, __LINE__, "`%s` died, child status %d", #stmt, _rc); } while (0)

/* Absolute path of a file under test/fixtures (static buffer; copy it if you keep it). */
static const char *fixture_path(const char *name)
{
    static char buf[4096];
    snprintf(buf, sizeof buf, "%s/%s", AVOS_TEST_FIXTURES, name);
    return buf;
}

static int avos_run_one(const avos_test_case *t)
{
    avos_checks = avos_failures = 0;
    avos_current = t->name;
    if (setjmp(avos_abort) == 0)
        t->fn();
    return avos_failures ? 1 : 0;
}

#ifndef AVOS_TEST_NO_MAIN
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--list")) {
        for (int i = 0; i < avos_test_count; i++) puts(avos_tests[i].name);
        return 0;
    }
    if (argc > 1) {                          /* run one test in this process (what pytest does) */
        for (int i = 0; i < avos_test_count; i++)
            if (!strcmp(avos_tests[i].name, argv[1])) {
                int rc = avos_run_one(&avos_tests[i]);
                fprintf(stderr, "%s: %d checks, %d failed\n", argv[1], avos_checks, avos_failures);
                return rc;
            }
        fprintf(stderr, "no such test: %s\n", argv[1]);
        return 2;
    }
    int failed = 0;                          /* run everything, each test in its own process */
    fflush(NULL);
    for (int i = 0; i < avos_test_count; i++) {
        pid_t pid = fork();
        if (pid == 0) exit(avos_run_one(&avos_tests[i]));
        int st = 0;
        waitpid(pid, &st, 0);
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        printf("%s %s\n", ok ? "PASS" : "FAIL", avos_tests[i].name);
        failed += !ok;
    }
    printf("%d tests, %d failed\n", avos_test_count, failed);
    return failed ? 1 : 0;
}
#endif
#endif /* AVOS_TEST_H */
