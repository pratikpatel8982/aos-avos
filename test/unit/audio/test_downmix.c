/*
 * Source/downmix.c -- multichannel -> stereo downmix (interleaved and planar; 16/24/32-bit integer
 * and 32/64-bit float input).
 *
 * The file is #included so the `nodm` debug switch (a static) is reachable. Expected values come from
 * a double-precision reference mixer written here from the documented ITU-R BS.775 coefficients
 * (centre/surround 0.7, LFE 0.5), not from the code's own expression.
 */
#include "avos_test.h"
#include "Source/downmix.c"

/* ---------------------------------------------------------------- reference ---------------- */

static const double REF_COEFF[8] = { 1.0, 1.0, 0.7, 0.5, 0.7, 0.7, 0.7, 0.7 };   /* FL FR CTR SUB BL BR SL SR */

static void ref_mix(const double in[8], double *l, double *r)
{
    *l = REF_COEFF[CH_FL] * in[CH_FL] + REF_COEFF[CH_CTR] * in[CH_CTR] + REF_COEFF[CH_SUB] * in[CH_SUB] +
         REF_COEFF[CH_BL] * in[CH_BL] + REF_COEFF[CH_SL] * in[CH_SL];
    *r = REF_COEFF[CH_FR] * in[CH_FR] + REF_COEFF[CH_CTR] * in[CH_CTR] + REF_COEFF[CH_SUB] * in[CH_SUB] +
         REF_COEFF[CH_BR] * in[CH_BR] + REF_COEFF[CH_SR] * in[CH_SR];
}

static int sat16(double v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int)v;                       /* truncation toward zero, like the C conversion in the code */
}

/* ---------------------------------------------------------------- encoders ----------------- */

static uint8_t *put16(uint8_t *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; return p + 2; }
static uint8_t *put24(uint8_t *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; return p + 3; }
static uint8_t *put32(uint8_t *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; return p + 4; }

#define SENTINEL 0x5a5a
static void fill_sentinel(uint16_t *pcm, int n) { for (int i = 0; i < n; i++) pcm[i] = SENTINEL; }
static int untouched(const uint16_t *pcm, int n) { for (int i = 0; i < n; i++) if (pcm[i] != SENTINEL) return 0; return 1; }

/* 5.1 order used by most containers: FL FR C LFE BL BR */
static int MAP_51[6] = { CH_FL, CH_FR, CH_CTR, CH_SUB, CH_BL, CH_BR };
static int MAP_71[8] = { CH_FL, CH_FR, CH_CTR, CH_SUB, CH_BL, CH_BR, CH_SL, CH_SR };
static int MAP_STEREO[2] = { CH_FL, CH_FR };

/* ---------------------------------------------------------------- tests -------------------- */

TEST(coefficient_table_follows_itu_bs775)
{
    CHECK_NEAR(downmix_coeff[CH_FL], 1.0, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_FR], 1.0, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_CTR], 0.7, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_SUB], 0.5, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_BL], 0.7, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_BR], 0.7, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_SL], 0.7, 1e-6);
    CHECK_NEAR(downmix_coeff[CH_SR], 0.7, 1e-6);
}

TEST(stereo_16bit_passes_through_unchanged)
{
    uint8_t src[4 * 4], *p = src;
    const int fr[4][2] = { { 0, 0 }, { 1000, -1000 }, { 32767, -32768 }, { -5, 7 } };
    for (int i = 0; i < 4; i++) { p = put16(p, fr[i][0]); p = put16(p, fr[i][1]); }
    uint16_t out[8];
    downmix(out, src, 4, 2, 16, MAP_STEREO);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_INT((int16_t)out[2 * i], fr[i][0]);
        CHECK_EQ_INT((int16_t)out[2 * i + 1], fr[i][1]);
    }
}

TEST(five_point_one_16bit_matches_the_reference_mix)
{
    const int frames[3][6] = {
        { 1000, 2000, 3000, 4000, 5000, 6000 },
        { -1000, 500, 20000, -8000, 100, -100 },
        { 0, 0, 0, 0, 0, 0 },
    };
    uint8_t src[3 * 6 * 2], *p = src;
    for (int f = 0; f < 3; f++) for (int c = 0; c < 6; c++) p = put16(p, frames[f][c]);
    uint16_t out[6];
    downmix(out, src, 3, 6, 16, MAP_51);
    for (int f = 0; f < 3; f++) {
        double in[8] = { 0 }, l, r;
        for (int c = 0; c < 6; c++) in[MAP_51[c]] = frames[f][c];
        ref_mix(in, &l, &r);
        CHECK_NEAR((int16_t)out[2 * f], sat16(l), 1);
        CHECK_NEAR((int16_t)out[2 * f + 1], sat16(r), 1);
    }
    /* spot-check the first frame by hand: L = 1000 + .7*3000 + .5*4000 + .7*5000 = 8600 */
    CHECK_NEAR((int16_t)out[0], 8600, 1);
    CHECK_NEAR((int16_t)out[1], 2000 + 2100 + 2000 + 4200, 1);        /* R = 10300 */
}

TEST(seven_point_one_includes_the_side_channels)
{
    const int in8[8] = { 100, 200, 300, 400, 500, 600, 700, 800 };
    uint8_t src[16], *p = src;
    for (int c = 0; c < 8; c++) p = put16(p, in8[c]);
    uint16_t out[2];
    downmix(out, src, 1, 8, 16, MAP_71);
    double in[8], l, r;
    for (int c = 0; c < 8; c++) in[MAP_71[c]] = in8[c];
    ref_mix(in, &l, &r);
    CHECK_NEAR((int16_t)out[0], sat16(l), 1);          /* 100 + 210 + 200 + 350 + 490 = 1350 */
    CHECK_NEAR((int16_t)out[1], sat16(r), 1);          /* 200 + 210 + 200 + 420 + 560 = 1590 */
    CHECK_NEAR((int16_t)out[0], 1350, 1);
    CHECK_NEAR((int16_t)out[1], 1590, 1);
}

TEST(channel_map_decides_which_input_goes_where)
{
    /* same bytes, swapped FL/FR map -> swapped output */
    uint8_t src[4], *p = src;
    p = put16(p, 1111);
    p = put16(p, -2222);
    int swapped[2] = { CH_FR, CH_FL };
    uint16_t out[2];
    downmix(out, src, 1, 2, 16, swapped);
    CHECK_EQ_INT((int16_t)out[0], -2222);
    CHECK_EQ_INT((int16_t)out[1], 1111);
}

TEST(sum_saturates_to_the_16bit_range)
{
    uint8_t src[12], *p = src;
    /* FL + .7 * CTR = 30000 + 21000 -> clamp to 32767; second frame: -30000 - 21000 -> -32768 */
    int map[2] = { CH_FL, CH_CTR };
    p = put16(p, 30000); p = put16(p, 30000);
    p = put16(p, -30000); p = put16(p, -30000);
    uint16_t out[4];
    downmix(out, src, 2, 2, 16, map);
    CHECK_EQ_INT((int16_t)out[0], 32767);
    CHECK_EQ_INT((int16_t)out[2], -32768);
    CHECK_EQ_INT((int16_t)out[1], 21000);                /* right only gets .7 * CTR */
    CHECK_EQ_INT((int16_t)out[3], -21000);
}

TEST(input_24bit_is_scaled_down_to_16_bits)
{
    uint8_t src[12], *p = src;
    p = put24(p, 0x123400);     /* 0x1234 << 8 */
    p = put24(p, -0x200000);    /* -0x2000 << 8 */
    p = put24(p, 0x7fffff);     /* full scale */
    p = put24(p, -0x800000);
    uint16_t out[4];
    downmix(out, src, 2, 2, 24, MAP_STEREO);
    CHECK_EQ_INT((int16_t)out[0], 0x1234);
    CHECK_EQ_INT((int16_t)out[1], -0x2000);
    CHECK_EQ_INT((int16_t)out[2], 0x7fff);
    CHECK_EQ_INT((int16_t)out[3], -0x8000);
}

TEST(input_32bit_is_scaled_down_to_16_bits)
{
    uint8_t src[16], *p = src;
    p = put32(p, 0x12340000);
    p = put32(p, -0x20000000);
    p = put32(p, 0x7fffff80);                 /* largest value a float holds exactly; see the known_defect test below */
    p = put32(p, (int)0x80000000);
    uint16_t out[4];
    downmix(out, src, 2, 2, 32, MAP_STEREO);
    CHECK_EQ_INT((int16_t)out[0], 0x1234);
    CHECK_EQ_INT((int16_t)out[1], -0x2000);
    CHECK_EQ_INT((int16_t)out[2], 0x7fff);
    CHECK_EQ_INT((int16_t)out[3], -0x8000);
}

/*
 * KNOWN DEFECT (production code, not fixed here): downmix() on 32-bit input accumulates in int32 and then
 * does `(int32_t)(coeff_float * ch)`. A float cannot hold 0x7fffffff (it rounds up to 2^31), and a sum of
 * several loud channels exceeds INT_MAX; converting such a float to int32_t is undefined behaviour and
 * yields INT_MIN on x86. The sample then comes out as -32768 instead of saturating to +32767, i.e. a loud
 * positive peak flips to full-scale negative. 16- and 24-bit input cannot reach this range.
 * These assertions pin the CURRENT result. When downmix.c is fixed they fail: change the expected values to
 * the `correct` ones below and delete the "known_defect_" prefix.
 */
TEST(known_defect_32bit_positive_overflow_wraps_instead_of_saturating)
{
    int map[2] = { CH_FL, CH_CTR };
    uint16_t o1[2];
    int map1[2] = { CH_FL, CH_FR };
    uint8_t two[8]; put32(two, 0x7fffffff); put32(two + 4, 0);
    downmix(o1, two, 1, 2, 32, map1);
    CHECK_EQ_INT((int16_t)o1[0], -32768);      /* correct: 32767 */

    uint8_t loud[8]; put32(loud, 0x60000000); put32(loud + 4, 0x60000000);
    uint16_t o2[2];
    downmix(o2, loud, 1, 2, 32, map);
    CHECK_EQ_INT((int16_t)o2[0], -32768);      /* correct: 32767 (1.7 * 0x60000000 > INT_MAX) */
    CHECK_EQ_INT((int16_t)o2[1], 17203);       /* the right channel (only 0.7 * C) is in range and correct */
}

TEST(mono_is_duplicated_to_both_channels)
{
    uint8_t src[6], *p = src;
    p = put16(p, 1234);
    p = put16(p, -4321);
    p = put16(p, 7);
    int map[1] = { CH_FL };
    uint16_t out[6];
    downmix(out, src, 3, 1, 16, map);
    const int want[3] = { 1234, -4321, 7 };
    for (int i = 0; i < 3; i++) {
        CHECK_EQ_INT((int16_t)out[2 * i], want[i]);
        CHECK_EQ_INT((int16_t)out[2 * i + 1], want[i]);
    }
}

TEST(mono_in_24_and_32_bit_is_scaled_and_duplicated)
{
    int map[1] = { CH_FL };
    uint8_t s24[3]; put24(s24, 0x345600);
    uint16_t o24[2];
    downmix(o24, s24, 1, 1, 24, map);
    CHECK_EQ_INT((int16_t)o24[0], 0x3456);
    CHECK_EQ_INT((int16_t)o24[1], 0x3456);
    uint8_t s32[4]; put32(s32, 0x56780000);
    uint16_t o32[2];
    downmix(o32, s32, 1, 1, 32, map);
    CHECK_EQ_INT((int16_t)o32[0], 0x5678);
    CHECK_EQ_INT((int16_t)o32[1], 0x5678);
}

TEST(mono_mapped_to_a_non_front_channel_is_silent)
{
    /* Current behaviour, and arguably a defect: the mono branch always emits ch[CH_FL], so a mono
     * stream whose map says CH_CTR (or anything but CH_FL) is downmixed to silence. */
    uint8_t src[2]; put16(src, 5000);
    int map[1] = { CH_CTR };
    uint16_t out[2] = { SENTINEL, SENTINEL };
    downmix(out, src, 1, 1, 16, map);
    CHECK_EQ_INT(out[0], 0);
    CHECK_EQ_INT(out[1], 0);
}

TEST(mono_with_invalid_map_skips_the_sample_and_stays_in_step)
{
    int bad[1] = { CH_UNMAPPED };
    int too_big[1] = { CH_SR + 1 };
    const int bits[3] = { 16, 24, 32 };
    for (int b = 0; b < 3; b++) {
        uint8_t src[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x7f };
        uint16_t o1[2] = { SENTINEL, SENTINEL }, o2[2] = { SENTINEL, SENTINEL }, o3[2] = { SENTINEL, SENTINEL };
        downmix(o1, src, 1, 1, bits[b], bad);
        downmix(o2, src, 1, 1, bits[b], too_big);
        downmix(o3, src, 1, 1, bits[b], NULL);
        CHECK_EQ_INT(o1[0] | o1[1], 0);
        CHECK_EQ_INT(o2[0] | o2[1], 0);
        CHECK_EQ_INT(o3[0] | o3[1], 0);
    }
}

TEST(unmapped_channels_are_consumed_but_not_mixed)
{
    /* 3 channels: FL, <unmapped>, FR. The middle sample must be skipped, not mixed, and the next
     * frame must still start at the right byte offset. */
    int map[3] = { CH_FL, CH_UNMAPPED, CH_FR };
    uint8_t src[12], *p = src;
    p = put16(p, 100); p = put16(p, 9999); p = put16(p, 300);
    p = put16(p, 400); p = put16(p, 9999); p = put16(p, 600);
    uint16_t out[4];
    downmix(out, src, 2, 3, 16, map);
    CHECK_EQ_INT((int16_t)out[0], 100);
    CHECK_EQ_INT((int16_t)out[1], 300);
    CHECK_EQ_INT((int16_t)out[2], 400);
    CHECK_EQ_INT((int16_t)out[3], 600);
}

TEST(invalid_map_entries_advance_by_the_sample_size_for_every_bit_depth)
{
    int map[2] = { CH_UNMAPPED, CH_FR };
    const int bits[3] = { 16, 24, 32 };
    for (int b = 0; b < 3; b++) {
        int bytes = bits[b] / 8;
        uint8_t src[16] = { 0 };
        /* frame 0: [skipped][FR=value]; frame 1 likewise. Put a distinct value in each FR slot. */
        uint8_t *p = src + bytes;
        if (bits[b] == 16) { put16(p, 0x0123); put16(p + 2 * bytes, 0x0456); }
        if (bits[b] == 24) { put24(p, 0x012300); put24(p + 2 * bytes, 0x045600); }
        if (bits[b] == 32) { put32(p, 0x01230000); put32(p + 2 * bytes, 0x04560000); }
        uint16_t out[4];
        downmix(out, src, 2, 2, bits[b], map);
        CHECK_EQ_INT((int16_t)out[0], 0);
        CHECK_EQ_INT((int16_t)out[1], 0x0123);
        CHECK_EQ_INT((int16_t)out[2], 0);
        CHECK_EQ_INT((int16_t)out[3], 0x0456);
    }
}

TEST(unsupported_bit_depth_contributes_nothing)
{
    uint8_t src[16]; memset(src, 0x7f, sizeof src);
    uint16_t out[2] = { SENTINEL, SENTINEL };
    downmix(out, src, 1, 2, 8, MAP_STEREO);
    CHECK_EQ_INT(out[0], 0);
    CHECK_EQ_INT(out[1], 0);
}

TEST(guards_leave_the_output_untouched)
{
    uint8_t src[64] = { 0 };
    uint16_t out[8];
    uint8_t *planes[2] = { src, src };

    fill_sentinel(out, 8);
    downmix(NULL, src, 1, 2, 16, MAP_STEREO);                 /* must not crash */
    downmix(out, NULL, 1, 2, 16, MAP_STEREO);
    downmix(out, src, 1, 9, 16, MAP_71);                      /* more than 8 channels   */
    downmix_planar(out, NULL, 1, 2, 16, MAP_STEREO);
    downmix_planar(NULL, planes, 1, 2, 16, MAP_STEREO);
    downmix_planar(out, planes, 1, 9, 16, MAP_71);
    downmix_float(out, NULL, 1, 2, 32, MAP_STEREO);
    downmix_float(NULL, src, 1, 2, 32, MAP_STEREO);
    downmix_float(out, src, 1, 9, 32, MAP_71);
    downmix_float_planar(out, NULL, 1, 2, 32, MAP_STEREO);
    downmix_float_planar(NULL, planes, 1, 2, 32, MAP_STEREO);
    downmix_float_planar(out, planes, 1, 9, 32, MAP_71);
    CHECK(untouched(out, 8));

    downmix(out, src, 0, 2, 16, MAP_STEREO);                  /* zero frames writes nothing */
    CHECK(untouched(out, 8));
}

/* ---------------------------------------------------------------- planar ------------------- */

TEST(planar_equals_interleaved_for_every_depth_and_layout)
{
    const int depths[3] = { 16, 24, 32 };
    const int nch[3] = { 2, 6, 8 };
    int *maps[3] = { MAP_STEREO, MAP_51, MAP_71 };
    enum { FRAMES = 5 };
    for (int d = 0; d < 3; d++) for (int l = 0; l < 3; l++) {
        int bytes = depths[d] / 8, ch = nch[l];
        uint8_t inter[FRAMES * 8 * 4], plane[8][FRAMES * 4];
        uint8_t *ip = inter, *pp[8];
        for (int c = 0; c < ch; c++) pp[c] = plane[c];
        for (int f = 0; f < FRAMES; f++) for (int c = 0; c < ch; c++) {
            int v = (f * 131 + c * 977 + 5) * (c & 1 ? -1 : 1) * (depths[d] == 16 ? 1 : depths[d] == 24 ? 256 : 65536);
            uint8_t tmp[4];
            if (depths[d] == 16) put16(tmp, v); else if (depths[d] == 24) put24(tmp, v); else put32(tmp, v);
            memcpy(ip, tmp, bytes); ip += bytes;
            memcpy(plane[c] + f * bytes, tmp, bytes);
        }
        uint16_t a[FRAMES * 2], b[FRAMES * 2];
        downmix(a, inter, FRAMES, ch, depths[d], maps[l]);
        downmix_planar(b, pp, FRAMES, ch, depths[d], maps[l]);
        CHECK_MSG(!memcmp(a, b, sizeof a), "planar != interleaved at %d bit, %d channels", depths[d], ch);
    }
}

TEST(planar_mono_duplicates_and_invalid_maps_skip)
{
    uint8_t ch0[6]; put16(ch0, 321); put16(ch0 + 2, -654); put16(ch0 + 4, 9);
    uint8_t *planes[1] = { ch0 };
    int map[1] = { CH_FL };
    uint16_t out[6];
    downmix_planar(out, planes, 3, 1, 16, map);
    CHECK_EQ_INT((int16_t)out[0], 321); CHECK_EQ_INT((int16_t)out[1], 321);
    CHECK_EQ_INT((int16_t)out[2], -654); CHECK_EQ_INT((int16_t)out[3], -654);
    CHECK_EQ_INT((int16_t)out[4], 9); CHECK_EQ_INT((int16_t)out[5], 9);

    int bad[1] = { CH_UNMAPPED };
    const int bits[3] = { 16, 24, 32 };
    for (int b = 0; b < 3; b++) {
        uint8_t big[16]; memset(big, 0x44, sizeof big);
        uint8_t *pl[1] = { big };
        uint16_t o[4] = { SENTINEL, SENTINEL, SENTINEL, SENTINEL };
        downmix_planar(o, pl, 2, 1, bits[b], bad);
        CHECK_EQ_INT(o[0] | o[1] | o[2] | o[3], 0);
    }
    uint8_t s24[3]; put24(s24, 0x345600);
    uint8_t *p24[1] = { s24 };
    uint16_t o24[2];
    downmix_planar(o24, p24, 1, 1, 24, map);
    CHECK_EQ_INT((int16_t)o24[0], 0x3456);
    uint8_t s32[4]; put32(s32, 0x56780000);
    uint8_t *p32[1] = { s32 };
    uint16_t o32[2];
    downmix_planar(o32, p32, 1, 1, 32, map);
    CHECK_EQ_INT((int16_t)o32[1], 0x5678);
}

TEST(planar_with_a_null_channel_pointer_writes_nothing)
{
    uint8_t good[8] = { 0 };
    uint8_t *planes[2] = { good, NULL };
    uint16_t out[4];
    fill_sentinel(out, 4);
    downmix_planar(out, planes, 2, 2, 16, MAP_STEREO);
    CHECK(untouched(out, 4));
    fill_sentinel(out, 4);
    downmix_float_planar(out, planes, 2, 2, 32, MAP_STEREO);
    CHECK(untouched(out, 4));
}

/* ---------------------------------------------------------------- float -------------------- */

static uint8_t *putf(uint8_t *p, float v) { memcpy(p, &v, 4); return p + 4; }
static uint8_t *putd(uint8_t *p, double v) { memcpy(p, &v, 8); return p + 8; }

TEST(float_stereo_scales_by_32768_and_rounds)
{
    uint8_t src[16], *p = src;
    p = putf(p, 0.5f); p = putf(p, -0.25f);
    p = putf(p, 0.0001f); p = putf(p, -0.0001f);       /* 3.2768 -> 3 */
    uint16_t out[4];
    downmix_float(out, src, 2, 2, 32, MAP_STEREO);
    CHECK_EQ_INT((int16_t)out[0], 16384);
    CHECK_EQ_INT((int16_t)out[1], -8192);
    CHECK_EQ_INT((int16_t)out[2], 3);
    CHECK_EQ_INT((int16_t)out[3], -3);
}

TEST(float_input_beyond_full_scale_saturates)
{
    uint8_t src[8], *p = src;
    p = putf(p, 4.0f); p = putf(p, -4.0f);
    uint16_t out[2];
    downmix_float(out, src, 1, 2, 32, MAP_STEREO);
    CHECK_EQ_INT((int16_t)out[0], 32767);
    CHECK_EQ_INT((int16_t)out[1], -32768);
}

TEST(double_five_point_one_matches_the_reference_mix)
{
    const double frame[6] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
    uint8_t src[48], *p = src;
    for (int c = 0; c < 6; c++) p = putd(p, frame[c]);
    uint16_t out[2];
    downmix_float(out, src, 1, 6, 64, MAP_51);
    double in[8] = { 0 }, l, r;
    for (int c = 0; c < 6; c++) in[MAP_51[c]] = lrint(frame[c] * 32768);
    ref_mix(in, &l, &r);
    CHECK_NEAR((int16_t)out[0], sat16(l), 1);
    CHECK_NEAR((int16_t)out[1], sat16(r), 1);
}

TEST(float_mono_and_invalid_maps)
{
    int map[1] = { CH_FL };
    uint8_t f32[4]; putf(f32, 0.5f);
    uint16_t o[2];
    downmix_float(o, f32, 1, 1, 32, map);
    CHECK_EQ_INT((int16_t)o[0], 16384);
    CHECK_EQ_INT((int16_t)o[1], 16384);
    uint8_t f64[8]; putd(f64, -0.5);
    downmix_float(o, f64, 1, 1, 64, map);
    CHECK_EQ_INT((int16_t)o[0], -16384);
    CHECK_EQ_INT((int16_t)o[1], -16384);

    int bad[1] = { CH_UNMAPPED };
    uint8_t big[16]; memset(big, 0x3f, sizeof big);
    const int bits[2] = { 32, 64 };
    for (int b = 0; b < 2; b++) {
        uint16_t z[2] = { SENTINEL, SENTINEL };
        downmix_float(z, big, 1, 1, bits[b], bad);
        CHECK_EQ_INT(z[0] | z[1], 0);
    }
    /* stereo: one unmapped channel is consumed (4/8 bytes) but not mixed, in both widths */
    int half[2] = { CH_UNMAPPED, CH_FR };
    uint8_t s32[8], *q = s32;
    q = putf(q, 0.9f); q = putf(q, 0.25f);
    uint16_t o32[2];
    downmix_float(o32, s32, 1, 2, 32, half);
    CHECK_EQ_INT((int16_t)o32[0], 0);
    CHECK_EQ_INT((int16_t)o32[1], 8192);
    uint8_t s64[16], *q2 = s64;
    q2 = putd(q2, 0.9); q2 = putd(q2, 0.25);
    uint16_t o64[2];
    downmix_float(o64, s64, 1, 2, 64, half);
    CHECK_EQ_INT((int16_t)o64[0], 0);
    CHECK_EQ_INT((int16_t)o64[1], 8192);
}

TEST(float_planar_equals_float_interleaved)
{
    const int depths[2] = { 32, 64 };
    for (int d = 0; d < 2; d++) {
        enum { FRAMES = 4, CH = 6 };
        uint8_t inter[FRAMES * CH * 8], plane[CH][FRAMES * 8];
        uint8_t *ip = inter, *pp[CH];
        for (int c = 0; c < CH; c++) pp[c] = plane[c];
        for (int f = 0; f < FRAMES; f++) for (int c = 0; c < CH; c++) {
            double v = ((f + 1) * 0.07 + c * 0.11) * (c & 1 ? -1 : 1);
            if (depths[d] == 32) { putf(ip, (float)v); putf(plane[c] + f * 4, (float)v); ip += 4; }
            else { putd(ip, v); putd(plane[c] + f * 8, v); ip += 8; }
        }
        uint16_t a[FRAMES * 2], b[FRAMES * 2];
        downmix_float(a, inter, FRAMES, CH, depths[d], MAP_51);
        downmix_float_planar(b, pp, FRAMES, CH, depths[d], MAP_51);
        CHECK_MSG(!memcmp(a, b, sizeof a), "float planar != interleaved at %d bit", depths[d]);
    }
}

TEST(float_planar_mono_stereo_and_invalid_map_branches)
{
    uint8_t m32[4]; putf(m32, 0.5f);
    uint8_t *pm[1] = { m32 };
    int fl[1] = { CH_FL };
    uint16_t o[2];
    downmix_float_planar(o, pm, 1, 1, 32, fl);
    CHECK_EQ_INT((int16_t)o[0], 16384);
    CHECK_EQ_INT((int16_t)o[1], 16384);

    uint8_t m64[8]; putd(m64, -0.5);
    uint8_t *pd[1] = { m64 };
    downmix_float_planar(o, pd, 1, 1, 64, fl);
    CHECK_EQ_INT((int16_t)o[0], -16384);

    int bad[1] = { CH_UNMAPPED };
    const int bits[2] = { 32, 64 };
    for (int b = 0; b < 2; b++) {
        uint8_t big[16]; memset(big, 0x3f, sizeof big);
        uint8_t *pl[1] = { big };
        uint16_t z[2] = { SENTINEL, SENTINEL };
        downmix_float_planar(z, pl, 1, 1, bits[b], bad);
        CHECK_EQ_INT(z[0] | z[1], 0);
    }

    int half[2] = { CH_UNMAPPED, CH_FR };
    uint8_t l32[4], r32[4]; putf(l32, 0.9f); putf(r32, 0.25f);
    uint8_t *p32[2] = { l32, r32 };
    downmix_float_planar(o, p32, 1, 2, 32, half);
    CHECK_EQ_INT((int16_t)o[0], 0);
    CHECK_EQ_INT((int16_t)o[1], 8192);
    uint8_t l64[8], r64[8]; putd(l64, 0.9); putd(r64, 0.25);
    uint8_t *p64[2] = { l64, r64 };
    downmix_float_planar(o, p64, 1, 2, 64, half);
    CHECK_EQ_INT((int16_t)o[0], 0);
    CHECK_EQ_INT((int16_t)o[1], 8192);
}

TEST(planar_integer_invalid_map_branches_for_multichannel)
{
    int half[2] = { CH_UNMAPPED, CH_FR };
    const int bits[3] = { 16, 24, 32 };
    for (int b = 0; b < 3; b++) {
        uint8_t l[4] = { 0x55, 0x55, 0x55, 0x55 }, r[4] = { 0 };
        if (bits[b] == 16) put16(r, 0x0777);
        if (bits[b] == 24) put24(r, 0x077700);
        if (bits[b] == 32) put32(r, 0x07770000);
        uint8_t *pl[2] = { l, r };
        uint16_t o[2];
        downmix_planar(o, pl, 1, 2, bits[b], half);
        CHECK_EQ_INT((int16_t)o[0], 0);
        CHECK_EQ_INT((int16_t)o[1], 0x0777);
    }
}

/* ---------------------------------------------------------------- debug switch ------------- */

TEST(the_nodm_debug_switch_disables_and_reenables_downmixing)
{
    uint8_t src[4], *p = src;
    p = put16(p, 1000); p = put16(p, -1000);
    uint16_t out[2];

    fill_sentinel(out, 2);
    _no_downmix();                                         /* the code behind the "nodm" debug command */
    CHECK_EQ_INT(no_downmix, 1);
    downmix(out, src, 1, 2, 16, MAP_STEREO);
    CHECK(untouched(out, 2));                              /* disabled: nothing written */
    uint8_t *planes[2] = { src, src + 2 };
    downmix_planar(out, planes, 1, 2, 16, MAP_STEREO);
    CHECK(untouched(out, 2));
    downmix_float(out, src, 1, 2, 32, MAP_STEREO);
    CHECK(untouched(out, 2));
    downmix_float_planar(out, planes, 1, 2, 32, MAP_STEREO);
    CHECK(untouched(out, 2));

    _no_downmix();
    CHECK_EQ_INT(no_downmix, 0);
    downmix(out, src, 1, 2, 16, MAP_STEREO);
    CHECK_EQ_INT((int16_t)out[0], 1000);
    CHECK_EQ_INT((int16_t)out[1], -1000);
}
