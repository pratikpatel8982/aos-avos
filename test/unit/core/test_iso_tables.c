/*
 * Covers Source/iso639.c (RFC 1766 primary language tag) and Source/iso3166.c (country sub-tag and
 * the country table accessors). Real object code from libavos_cov.a, driven via its public API.
 */
#include "avos_test.h"
#include "iso639.h"
#include "types.h"

/* iso3166.c has no header of its own. */
const char *map_ISO3166_code( const char *code );
const char *map_ISO3166_getCountryFromCode( const char *P_ptCode, int *P_ptIndex );
INT32 map_ISO3166_getCountryListSize( void );
void map_ISO3166_getCountryFromIndex( INT32 P_Index, const char **P_ptptCode, const char **P_ptptName );

/* ---------------------------------------------------------------- ISO 639 */

TEST(language_tag_with_region_returns_lowercased_primary_tag)
{
    CHECK_EQ_STR(map_ISO639_code("en-US"), "en");
    CHECK_EQ_STR(map_ISO639_code("fr-CA"), "fr");
    CHECK_EQ_STR(map_ISO639_code("zh-Hant-TW"), "zh");
}

TEST(language_tag_is_lowercased_regardless_of_input_case)
{
    CHECK_EQ_STR(map_ISO639_code("EN"), "en");
    CHECK_EQ_STR(map_ISO639_code("eNg"), "eng");
    CHECK_EQ_STR(map_ISO639_code("DE-at"), "de");
}

TEST(empty_language_tag_maps_to_empty_string)
{
    CHECK_EQ_STR(map_ISO639_code(""), "");
}

TEST(language_tag_without_leading_letters_is_returned_unchanged)
{
    const char *in1 = "123";
    const char *in2 = "-US";
    const char *in3 = " en";
    CHECK(map_ISO639_code(in1) == in1);           /* same pointer: "no mapping, original returned" */
    CHECK(map_ISO639_code(in2) == in2);
    CHECK(map_ISO639_code(in3) == in3);
}

TEST(language_tag_with_non_dash_separator_is_returned_unchanged)
{
    const char *in = "en_US";
    CHECK(map_ISO639_code(in) == in);
    const char *in2 = "en1";
    CHECK(map_ISO639_code(in2) == in2);
}

TEST(language_tag_primary_part_is_capped_at_eight_letters)
{
    CHECK_EQ_STR(map_ISO639_code("abcdefgh"), "abcdefgh");
    CHECK_EQ_STR(map_ISO639_code("ABCDEFGHIJKL"), "abcdefgh");   /* longer than 8 -> truncated */
    CHECK_EQ_STR(map_ISO639_code("abcdefghi-US"), "abcdefgh");
}

TEST(language_tag_trailing_dash_yields_primary_only)
{
    CHECK_EQ_STR(map_ISO639_code("de-"), "de");
    CHECK_EQ_STR(map_ISO639_code("x"), "x");
}

TEST(language_tag_result_buffer_is_reused_between_calls)
{
    const char *a = map_ISO639_code("en-US");
    const char *b = map_ISO639_code("fr-CA");
    CHECK(a == b);                       /* documented static buffer */
    CHECK_EQ_STR(b, "fr");
}

/* --------------------------------------------------------------- ISO 3166 */

TEST(country_subtag_maps_to_full_upper_case_name)
{
    CHECK_EQ_STR(map_ISO3166_code("en-US"), "UNITED STATES");
    CHECK_EQ_STR(map_ISO3166_code("fr-FR"), "FRANCE");
    CHECK_EQ_STR(map_ISO3166_code("en-GB"), "UNITED KINGDOM");
    CHECK_EQ_STR(map_ISO3166_code("sn-ZW"), "ZIMBABWE");
    CHECK_EQ_STR(map_ISO3166_code("ps-AF"), "AFGHANISTAN");
}

TEST(country_subtag_lookup_is_case_insensitive)
{
    CHECK_EQ_STR(map_ISO3166_code("en-us"), "UNITED STATES");
    CHECK_EQ_STR(map_ISO3166_code("fr-Fr"), "FRANCE");
}

TEST(country_subtag_stops_at_next_dash)
{
    CHECK_EQ_STR(map_ISO3166_code("zh-TW-x"), "TAIWAN, POC");
}

TEST(language_without_country_part_gives_empty_string)
{
    CHECK_EQ_STR(map_ISO3166_code("en"), "");
    CHECK_EQ_STR(map_ISO3166_code(""), "");
}

TEST(language_with_empty_country_part_gives_empty_string)
{
    CHECK_EQ_STR(map_ISO3166_code("en-"), "");
    CHECK_EQ_STR(map_ISO3166_code("en--US"), "");
}

TEST(unknown_country_subtag_is_returned_as_typed)
{
    CHECK_EQ_STR(map_ISO3166_code("en-XX"), "XX");
    CHECK_EQ_STR(map_ISO3166_code("en-Latn"), "Latn");        /* 4-letter script subtag, not a country */
    CHECK_EQ_STR(map_ISO3166_code("en-U"), "U");              /* shorter than a 2-letter code */
}

TEST(country_subtag_is_capped_at_eight_characters)
{
    CHECK_EQ_STR(map_ISO3166_code("en-ABCDEFGHIJKL"), "ABCDEFGH");
}

TEST(country_list_size_matches_enumeration_and_is_stable)
{
    INT32 n = map_ISO3166_getCountryListSize();
    CHECK(n >= 240);                      /* ISO 3166-1 has 249 assigned codes */
    CHECK(n < 300);
    const char *code = NULL, *name = NULL;
    map_ISO3166_getCountryFromIndex(0, &code, &name);
    CHECK_EQ_STR(code, "AF");
    CHECK_EQ_STR(name, "AFGHANISTAN");
    map_ISO3166_getCountryFromIndex(n - 1, &code, &name);
    CHECK_EQ_STR(code, "ZW");
    CHECK_EQ_STR(name, "ZIMBABWE");
}

TEST(every_table_entry_has_two_letter_code_and_name_and_roundtrips_by_code)
{
    INT32 n = map_ISO3166_getCountryListSize();
    for (INT32 i = 0; i < n; i++) {
        const char *code = NULL, *name = NULL;
        map_ISO3166_getCountryFromIndex(i, &code, &name);
        REQUIRE_NOT_NULL(code);
        REQUIRE_NOT_NULL(name);
        CHECK_EQ_INT(strlen(code), 2);
        CHECK(strlen(name) > 0);
        int idx = -1;
        const char *found = map_ISO3166_getCountryFromCode(code, &idx);
        CHECK(found == name);
        CHECK_EQ_INT(idx, i);
    }
}

TEST(country_index_out_of_range_leaves_outputs_untouched)
{
    const char *code = "keep-code", *name = "keep-name";
    map_ISO3166_getCountryFromIndex(map_ISO3166_getCountryListSize(), &code, &name);
    CHECK_EQ_STR(code, "keep-code");
    CHECK_EQ_STR(name, "keep-name");
    map_ISO3166_getCountryFromIndex(100000, &code, &name);
    CHECK_EQ_STR(code, "keep-code");
    CHECK_EQ_STR(name, "keep-name");
}

TEST(country_from_code_is_case_insensitive_and_sets_index)
{
    int idx = -1;
    CHECK_EQ_STR(map_ISO3166_getCountryFromCode("fr", &idx), "FRANCE");
    const char *code = NULL, *name = NULL;
    map_ISO3166_getCountryFromIndex(idx, &code, &name);
    CHECK_EQ_STR(code, "FR");
}

TEST(country_from_unknown_code_returns_null_and_keeps_index)
{
    int idx = 1234;
    CHECK_NULL(map_ISO3166_getCountryFromCode("QQ", &idx));
    CHECK_EQ_INT(idx, 1234);
    CHECK_NULL(map_ISO3166_getCountryFromCode("", &idx));
    CHECK_EQ_INT(idx, 1234);
}
