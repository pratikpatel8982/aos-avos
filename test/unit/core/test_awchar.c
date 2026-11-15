/*
 * Covers Source/awchar.c: the 16-bit wide-char string helpers (wchar = unsigned short).
 * Real object code from libavos_cov.a.
 */
#include "avos_test.h"
#include "awchar.h"
#include "astdlib.h"

static wchar *W(const char *s, wchar *dst) { c2wstrcpy(dst, s); return dst; }

TEST(wstrlen_counts_units_before_terminator)
{
    wchar a[8] = {0};
    CHECK_EQ_INT(wstrlen(a), 0);
    wchar b[8] = {'a', 'b', 'c', 0};
    CHECK_EQ_INT(wstrlen(b), 3);
    wchar c[4] = {0x4e2d, 0xffff, 0x00e9, 0};          /* non-ASCII and high values count as one unit each */
    CHECK_EQ_INT(wstrlen(c), 3);
}

TEST(wstrcpy_copies_including_terminator_and_returns_dest)
{
    wchar src[8], dst[8];
    W("hello", src);
    memset(dst, 0x7f, sizeof dst);
    CHECK(wstrcpy(dst, src) == dst);
    CHECK_EQ_INT(wstrlen(dst), 5);
    CHECK(!memcmp(dst, src, 6 * sizeof(wchar)));
    CHECK_EQ_INT(dst[6], 0x7f7f);                      /* nothing written past the terminator */
}

TEST(wstrcpy_of_empty_string_writes_only_terminator)
{
    wchar src[2] = {0}, dst[3] = {1, 2, 3};
    wstrcpy(dst, src);
    CHECK_EQ_INT(dst[0], 0);
    CHECK_EQ_INT(dst[1], 2);
}

TEST(c2wstrcpy_widens_each_byte_without_sign_extension_of_ascii)
{
    wchar dst[8];
    c2wstrcpy(dst, "abc");
    CHECK_EQ_INT(dst[0], 'a');
    CHECK_EQ_INT(dst[1], 'b');
    CHECK_EQ_INT(dst[2], 'c');
    CHECK_EQ_INT(dst[3], 0);
}

TEST(c2wstrcpy_widens_high_bytes_with_sign_extension_of_signed_char)
{
    /* Documented-by-behaviour: (wchar)*char with a signed char 0xE9 becomes 0xFFE9 on this target. */
    wchar dst[4];
    const char s[] = {(char)0xE9, 0};
    c2wstrcpy(dst, s);
    CHECK_EQ_INT(dst[0], (wchar)(char)0xE9);
    CHECK_EQ_INT(dst[1], 0);
}

TEST(wstrdup_allocates_an_independent_copy)
{
    wchar src[8];
    W("dup me", src);
    wchar *d = wstrdup(src);
    REQUIRE_NOT_NULL(d);
    CHECK(d != src);
    CHECK_EQ_INT(wstrlen(d), 6);
    CHECK(!memcmp(d, src, 7 * sizeof(wchar)));
    src[0] = 'X';
    CHECK_EQ_INT(d[0], 'd');
    afree(d);
}

TEST(wstrdup_of_empty_string_gives_empty_string)
{
    wchar e[1] = {0};
    wchar *d = wstrdup(e);
    REQUIRE_NOT_NULL(d);
    CHECK_EQ_INT(d[0], 0);
    afree(d);
}

TEST(wstrncpy_pads_with_zeros_when_source_is_shorter)
{
    wchar src[4], dst[8];
    W("ab", src);
    memset(dst, 0x7f, sizeof dst);
    CHECK(wstrncpy(dst, src, 6) == dst);
    CHECK_EQ_INT(dst[0], 'a');
    CHECK_EQ_INT(dst[1], 'b');
    for (int i = 2; i < 6; i++) CHECK_EQ_INT(dst[i], 0);
    CHECK_EQ_INT(dst[6], 0x7f7f);                      /* exactly n units touched */
}

TEST(wstrncpy_does_not_terminate_when_source_is_at_least_n_long)
{
    wchar src[8], dst[8];
    W("abcdef", src);
    memset(dst, 0x7f, sizeof dst);
    wstrncpy(dst, src, 3);
    CHECK_EQ_INT(dst[0], 'a');
    CHECK_EQ_INT(dst[1], 'b');
    CHECK_EQ_INT(dst[2], 'c');
    CHECK_EQ_INT(dst[3], 0x7f7f);
}

TEST(wstrncpy_with_source_exactly_n_minus_one_long_terminates_without_padding_loop)
{
    wchar src[8], dst[8];
    W("ab", src);
    memset(dst, 0x7f, sizeof dst);
    wstrncpy(dst, src, 3);                             /* copies a, b, NUL: n reaches 0 on the NUL copy */
    CHECK_EQ_INT(dst[2], 0);
    CHECK_EQ_INT(dst[3], 0x7f7f);
}

TEST(wstrncpy_with_zero_count_leaves_destination_untouched)
{
    wchar src[4], dst[4];
    W("abc", src);
    memset(dst, 0x7f, sizeof dst);
    CHECK(wstrncpy(dst, src, 0) == dst);
    CHECK_EQ_INT(dst[0], 0x7f7f);
}

TEST(c2wstrncpy_pads_truncates_and_ignores_zero_count)
{
    wchar dst[8];
    memset(dst, 0x7f, sizeof dst);
    c2wstrncpy(dst, "ab", 5);
    CHECK_EQ_INT(dst[0], 'a'); CHECK_EQ_INT(dst[1], 'b');
    for (int i = 2; i < 5; i++) CHECK_EQ_INT(dst[i], 0);
    CHECK_EQ_INT(dst[5], 0x7f7f);

    memset(dst, 0x7f, sizeof dst);
    c2wstrncpy(dst, "abcdef", 3);
    CHECK_EQ_INT(dst[2], 'c');
    CHECK_EQ_INT(dst[3], 0x7f7f);                      /* truncated copy is not terminated */

    memset(dst, 0x7f, sizeof dst);
    CHECK(c2wstrncpy(dst, "abc", 0) == dst);
    CHECK_EQ_INT(dst[0], 0x7f7f);
}

TEST(wstrcat_appends_and_returns_first_argument)
{
    wchar d[16], s[8];
    W("foo", d);
    W("bar", s);
    CHECK(wstrcat(d, s) == d);
    CHECK_EQ_INT(wstrlen(d), 6);
    CHECK_EQ_INT(d[3], 'b');
    CHECK_EQ_INT(d[5], 'r');
    CHECK_EQ_INT(d[6], 0);
}

TEST(wstrcat_with_empty_operands)
{
    wchar d[8], e[1] = {0}, s[4];
    W("x", d);
    wstrcat(d, e);
    CHECK_EQ_INT(wstrlen(d), 1);
    d[0] = 0;
    W("yz", s);
    wstrcat(d, s);
    CHECK_EQ_INT(wstrlen(d), 2);
    CHECK_EQ_INT(d[0], 'y');
}

TEST(c2wstrcat_appends_narrow_string)
{
    wchar d[16];
    W("ab", d);
    CHECK(c2wstrcat(d, "cd") == d);
    CHECK_EQ_INT(wstrlen(d), 4);
    CHECK_EQ_INT(d[2], 'c');
    CHECK_EQ_INT(d[3], 'd');
    c2wstrcat(d, "");
    CHECK_EQ_INT(wstrlen(d), 4);
}

TEST(c2wstrncat_appends_at_most_n_and_terminates)
{
    wchar d[16];
    memset(d, 0x7f, sizeof d);
    W("ab", d);
    wchar *r = c2wstrncat(d, "cdefg", 3);              /* truncated: NUL is added after 3 units */
    CHECK(r == d);
    CHECK_EQ_INT(wstrlen(d), 5);
    CHECK_EQ_INT(d[4], 'e');
    CHECK_EQ_INT(d[5], 0);
}

TEST(c2wstrncat_with_short_source_returns_pointer_to_terminator)
{
    wchar d[16];
    W("ab", d);
    wchar *r = c2wstrncat(d, "c", 5);
    CHECK(r == d + 3);                                  /* NOTE: returns the end, not string1, on this path */
    CHECK_EQ_INT(*r, 0);
    CHECK_EQ_INT(wstrlen(d), 3);
}

TEST(c2wstrncat_with_zero_count_is_a_noop)
{
    wchar d[8];
    W("ab", d);
    CHECK(c2wstrncat(d, "zzz", 0) == d);
    CHECK_EQ_INT(wstrlen(d), 2);
}

TEST(wstrncat_appends_at_most_n_and_terminates)
{
    wchar d[16], s[8];
    W("ab", d);
    W("cdefg", s);
    CHECK(wstrncat(d, s, 3) == d);
    CHECK_EQ_INT(wstrlen(d), 5);
    CHECK_EQ_INT(d[4], 'e');
    CHECK_EQ_INT(d[5], 0);
}

TEST(wstrncat_with_short_source_and_zero_count)
{
    wchar d[16], s[4];
    W("ab", d);
    W("c", s);
    CHECK(wstrncat(d, s, 9) == d);
    CHECK_EQ_INT(wstrlen(d), 3);
    CHECK_EQ_INT(d[2], 'c');
    CHECK(wstrncat(d, s, 0) == d);
    CHECK_EQ_INT(wstrlen(d), 3);
}

TEST(wstrchr_finds_first_occurrence_or_null)
{
    wchar s[8];
    W("abcabc", s);
    CHECK(wstrchr(s, 'b') == s + 1);
    CHECK(wstrchr(s, 'a') == s);
    CHECK(wstrchr(s, 'c') == s + 2);
    CHECK_NULL(wstrchr(s, 'z'));
    CHECK(wstrchr(s, 0) == s + 6);                      /* terminator is findable, like strchr */
    wchar e[1] = {0};
    CHECK_NULL(wstrchr(e, 'a'));
    wchar w[3] = {0x4e2d, 0x6587, 0};
    CHECK(wstrchr(w, 0x6587) == w + 1);
}

TEST(w2c_narrows_each_unit_to_a_byte)
{
    wchar w[4] = {'o', 'k', 0x0141, 0};                 /* 0x141 narrows to 0x41 */
    CHECK_EQ_STR(w2c(w), "okA");
    wchar e[1] = {0};
    CHECK_EQ_STR(w2c(e), "");
}

TEST(w2c_truncates_to_256_characters_and_reuses_buffer)
{
    wchar big[400];
    for (int i = 0; i < 399; i++) big[i] = 'a' + (i % 26);
    big[399] = 0;
    const char *r = w2c(big);
    CHECK_EQ_INT(strlen(r), 256);
    CHECK_EQ_INT(r[255], 'a' + (255 % 26));
    wchar s[2] = {'q', 0};
    const char *r2 = w2c(s);
    CHECK(r == r2);
    CHECK_EQ_STR(r2, "q");
}
