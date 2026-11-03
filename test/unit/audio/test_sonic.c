/*
 * Source/sonic.c -- Sonic time-stretching / pitch / rate / volume DSP, driven through its public API
 * (real object code from libavos_cov.a) with synthetic sine input.  Covers every sample format
 * (short / float / unsigned char), the PICOLA speed-up and slow-down branches, the sinc rate
 * resampler, buffer growth, parameter clamping, flush, the one-shot helpers, and the allocation
 * failure paths (reached with a real RLIMIT_AS, not a fake allocator).
 */
#include "avos_test.h"
#include "audio_pcm_helpers.h"
#include "sonic.h"

#include <limits.h>
#include <sys/resource.h>

typedef struct {
    short *pcm;       /* interleaved output */
    int frames;
} Out;

/* Run `in` through a stream configured by the arguments; feed `chunk` frames at a time, read
 * everything after every write, then flush and read the tail. */
static Out run(const short *in, int frames, int sr, int ch, float speed, float pitch, float rate,
               float volume, int quality, int chunk)
{
    sonicStream s = sonicCreateStream(sr, ch);
    Out o = { malloc((size_t)(frames * 40 + 400000) * ch * sizeof(short)), 0 };
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, speed);
    sonicSetPitch(s, pitch);
    sonicSetRate(s, rate);
    sonicSetVolume(s, volume);
    sonicSetQuality(s, quality);
    for (int pos = 0; pos < frames; pos += chunk) {
        int n = frames - pos < chunk ? frames - pos : chunk;
        REQUIRE(sonicWriteShortToStream(s, in + (size_t)pos * ch, n) == 1);
        o.frames += sonicReadShortFromStream(s, o.pcm + (size_t)o.frames * ch, 1 << 22);
    }
    REQUIRE(sonicFlushStream(s) == 1);
    o.frames += sonicReadShortFromStream(s, o.pcm + (size_t)o.frames * ch, 1 << 22);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 0);
    sonicDestroyStream(s);
    return o;
}

static Out run_default(const short *in, int frames, int sr, int ch, float speed, float pitch, float rate,
                       float volume)
{
    return run(in, frames, sr, ch, speed, pitch, rate, volume, 0, 700);
}

/* ------------------------------------------------------------------ construction / parameters */

TEST(a_new_stream_has_neutral_defaults_and_the_requested_format)
{
    sonicStream s = sonicCreateStream(44100, 2);
    REQUIRE_NOT_NULL(s);
    CHECK_EQ_INT(sonicGetSampleRate(s), 44100);
    CHECK_EQ_INT(sonicGetNumChannels(s), 2);
    CHECK_NEAR(sonicGetSpeed(s), 1.0f, 0);
    CHECK_NEAR(sonicGetPitch(s), 1.0f, 0);
    CHECK_NEAR(sonicGetRate(s), 1.0f, 0);
    CHECK_NEAR(sonicGetVolume(s), 1.0f, 0);
    CHECK_NEAR(sonicProcessingSpeed(s), 1.0f, 0);
    CHECK_EQ_INT(sonicGetQuality(s), 0);
    CHECK_EQ_INT(sonicGetChordPitch(s), 0);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 0);
    CHECK_EQ_INT(sonicInputSamplesBuffered(s), 0);
    CHECK_NULL(sonicGetUserData(s));
    sonicDestroyStream(s);
}

TEST(sample_rate_and_channel_count_are_clamped_at_creation)
{
    sonicStream s = sonicCreateStream(10, 0);
    REQUIRE_NOT_NULL(s);
    CHECK_EQ_INT(sonicGetSampleRate(s), SONIC_MIN_SAMPLE_RATE);
    CHECK_EQ_INT(sonicGetNumChannels(s), SONIC_MIN_CHANNELS);
    sonicDestroyStream(s);

    s = sonicCreateStream(10 * 1000 * 1000, 99);
    REQUIRE_NOT_NULL(s);
    CHECK_EQ_INT(sonicGetSampleRate(s), SONIC_MAX_SAMPLE_RATE);
    CHECK_EQ_INT(sonicGetNumChannels(s), SONIC_MAX_CHANNELS);
    sonicDestroyStream(s);
}

TEST(setters_store_in_range_values_and_clamp_out_of_range_ones)
{
    sonicStream s = sonicCreateStream(16000, 1);
    REQUIRE_NOT_NULL(s);

    sonicSetSpeed(s, 1.75f);   CHECK_NEAR(sonicGetSpeed(s), 1.75f, 1e-6);
    sonicSetSpeed(s, 0.0f);    CHECK_NEAR(sonicGetSpeed(s), SONIC_MIN_SPEED, 1e-6);
    sonicSetSpeed(s, -3.0f);   CHECK_NEAR(sonicGetSpeed(s), SONIC_MIN_SPEED, 1e-6);
    sonicSetSpeed(s, 1000.0f); CHECK_NEAR(sonicGetSpeed(s), SONIC_MAX_SPEED, 1e-6);

    sonicSetPitch(s, 1.25f);   CHECK_NEAR(sonicGetPitch(s), 1.25f, 1e-6);
    sonicSetPitch(s, 0.0f);    CHECK_NEAR(sonicGetPitch(s), SONIC_MIN_PITCH_SETTING, 1e-6);
    sonicSetPitch(s, 1e9f);    CHECK_NEAR(sonicGetPitch(s), SONIC_MAX_PITCH_SETTING, 1e-6);

    sonicSetRate(s, 0.8f);     CHECK_NEAR(sonicGetRate(s), 0.8f, 1e-6);
    sonicSetRate(s, 0.0f);     CHECK_NEAR(sonicGetRate(s), SONIC_MIN_RATE, 1e-6);
    sonicSetRate(s, 1e9f);     CHECK_NEAR(sonicGetRate(s), SONIC_MAX_RATE, 1e-6);

    sonicSetVolume(s, 0.5f);   CHECK_NEAR(sonicGetVolume(s), 0.5f, 1e-6);
    sonicSetVolume(s, 0.0f);   CHECK_NEAR(sonicGetVolume(s), SONIC_MIN_VOLUME, 1e-6);
    sonicSetVolume(s, 1e6f);   CHECK_NEAR(sonicGetVolume(s), SONIC_MAX_VOLUME, 1e-6);

    sonicSetQuality(s, 7);     CHECK_EQ_INT(sonicGetQuality(s), 1);
    sonicSetQuality(s, -1);    CHECK_EQ_INT(sonicGetQuality(s), 1);
    sonicSetQuality(s, 0);     CHECK_EQ_INT(sonicGetQuality(s), 0);

    sonicSetChordPitch(s, 1);  CHECK_EQ_INT(sonicGetChordPitch(s), 0);   /* deprecated no-op */
    sonicDestroyStream(s);
}

TEST(user_data_pointer_round_trips)
{
    sonicStream s = sonicCreateStream(8000, 1);
    int token = 42;
    REQUIRE_NOT_NULL(s);
    sonicSetUserData(s, &token);
    CHECK(sonicGetUserData(s) == &token);
    sonicSetUserData(s, NULL);
    CHECK_NULL(sonicGetUserData(s));
    sonicDestroyStream(s);
}

/* ------------------------------------------------------------------ pass-through */

TEST(neutral_settings_pass_the_samples_through_unchanged)
{
    const int n = 5000;
    short *in = pcm_sine1(n, 22050, 440, 12000);
    sonicStream s = sonicCreateStream(22050, 1);
    short *out = malloc(n * sizeof(short));
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    CHECK_EQ_INT(sonicSamplesAvailable(s), n);         /* speed==1: everything is copied straight out */
    CHECK_EQ_INT(sonicInputSamplesBuffered(s), 0);
    CHECK_EQ_INT(sonicReadShortFromStream(s, out, n), n);
    CHECK_EQ_MEM(in, out, n * sizeof(short));
    CHECK_EQ_INT(sonicReadShortFromStream(s, out, n), 0);   /* nothing left */
    sonicDestroyStream(s);
    free(in); free(out);
}

TEST(writing_zero_samples_is_a_successful_no_op)
{
    sonicStream s = sonicCreateStream(8000, 1);
    short x = 5;
    float f = 0.5f;
    unsigned char u = 200;
    REQUIRE_NOT_NULL(s);
    CHECK_EQ_INT(sonicWriteShortToStream(s, &x, 0), 1);
    CHECK_EQ_INT(sonicWriteFloatToStream(s, &f, 0), 1);
    CHECK_EQ_INT(sonicWriteUnsignedCharToStream(s, &u, 0), 1);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 0);
    CHECK_EQ_INT(sonicInputSamplesBuffered(s), 0);
    sonicDestroyStream(s);
}

TEST(reading_from_an_empty_stream_returns_zero_in_every_format)
{
    sonicStream s = sonicCreateStream(8000, 1);
    short sh[4]; float fl[4]; unsigned char uc[4];
    REQUIRE_NOT_NULL(s);
    CHECK_EQ_INT(sonicReadShortFromStream(s, sh, 4), 0);
    CHECK_EQ_INT(sonicReadFloatFromStream(s, fl, 4), 0);
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, uc, 4), 0);
    sonicDestroyStream(s);
}

/* ------------------------------------------------------------------ sample formats */

TEST(float_samples_are_scaled_by_32767_in_and_out)
{
    sonicStream s = sonicCreateStream(8000, 1);
    const float in[5] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.25f };
    float out[5];
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteFloatToStream(s, in, 5) == 1);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 5);
    CHECK_EQ_INT(sonicReadFloatFromStream(s, out, 5), 5);
    for (int i = 0; i < 5; i++) CHECK_NEAR(out[i], in[i], 1.0 / 32767.0);
    sonicDestroyStream(s);
}

TEST(float_write_then_short_read_gives_the_16_bit_integers)
{
    sonicStream s = sonicCreateStream(8000, 1);
    const float in[4] = { 1.0f, -1.0f, 0.5f, 0.0f };
    short out[4];
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteFloatToStream(s, in, 4) == 1);
    CHECK_EQ_INT(sonicReadShortFromStream(s, out, 4), 4);
    CHECK_EQ_INT(out[0], 32767);
    CHECK_EQ_INT(out[1], -32767);
    CHECK_EQ_INT(out[2], 16383);      /* 0.5 * 32767 truncated */
    CHECK_EQ_INT(out[3], 0);
    sonicDestroyStream(s);
}

TEST(unsigned_char_samples_are_recentred_and_shifted_to_16_bit)
{
    sonicStream s = sonicCreateStream(8000, 1);
    const unsigned char in[5] = { 128, 255, 0, 192, 64 };
    short sh[5];
    unsigned char back[5];
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteUnsignedCharToStream(s, in, 5) == 1);
    CHECK_EQ_INT(sonicReadShortFromStream(s, sh, 5), 5);
    CHECK_EQ_INT(sh[0], 0);
    CHECK_EQ_INT(sh[1], 127 << 8);
    CHECK_EQ_INT(sh[2], -128 << 8);
    CHECK_EQ_INT(sh[3], 64 << 8);
    CHECK_EQ_INT(sh[4], -64 << 8);

    REQUIRE(sonicWriteUnsignedCharToStream(s, in, 5) == 1);
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, back, 5), 5);
    CHECK_EQ_MEM(in, back, 5);        /* 8-bit round trip is lossless */
    sonicDestroyStream(s);
}

TEST(partial_unsigned_char_read_keeps_the_rest_in_order)
{
    sonicStream s = sonicCreateStream(8000, 1);
    const unsigned char in[6] = { 128, 255, 0, 192, 64, 100 };
    unsigned char a[2], b[4], c[4];
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteUnsignedCharToStream(s, in, 6) == 1);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 6);
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, a, 2), 2);   /* fewer than available */
    CHECK_EQ_MEM(a, in, 2);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 4);                   /* the other 4 were not lost */
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, b, 4), 4);
    CHECK_EQ_MEM(b, in + 2, 4);                                  /* ...and are still in order */
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, c, 4), 0);   /* now empty */
    sonicDestroyStream(s);
}

TEST(partial_unsigned_char_read_moves_whole_stereo_frames)
{
    sonicStream s = sonicCreateStream(8000, 2);
    const unsigned char in[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };   /* 4 frames, L/R interleaved */
    unsigned char a[2], rest[6];
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteUnsignedCharToStream(s, in, 4) == 1);
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, a, 1), 1);        /* ONE frame = two values */
    CHECK_EQ_MEM(a, in, 2);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 3);
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, rest, 3), 3);
    CHECK_EQ_MEM(rest, in + 2, 6);                                    /* remaining frames not shifted by a sample */
    sonicDestroyStream(s);
}

TEST(stereo_frames_stay_interleaved_through_every_format)
{
    sonicStream s = sonicCreateStream(8000, 2);
    const short in[8] = { 1, -1, 2, -2, 3, -3, 4, -4 };
    short out[8];
    REQUIRE_NOT_NULL(s);
    REQUIRE(sonicWriteShortToStream(s, in, 4) == 1);
    CHECK_EQ_INT(sonicReadShortFromStream(s, out, 4), 4);
    CHECK_EQ_MEM(in, out, sizeof in);
    sonicDestroyStream(s);
}

/* ------------------------------------------------------------------ partial reads */

TEST(partial_reads_return_the_stream_in_order_in_all_three_formats)
{
    const int n = 3000;
    short *in = pcm_sine1(n, 8000, 300, 9000);
    short *got = malloc(n * sizeof(short));
    float *fl = malloc(n * sizeof(float));
    unsigned char *uc = malloc(n);
    sonicStream s = sonicCreateStream(8000, 1);
    REQUIRE_NOT_NULL(s);

    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    CHECK_EQ_INT(sonicReadShortFromStream(s, got, 1000), 1000);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 2000);
    CHECK_EQ_INT(sonicReadFloatFromStream(s, fl, 1000), 1000);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 1000);
    CHECK_EQ_INT(sonicReadUnsignedCharFromStream(s, uc, 5000), 1000);   /* asks for more than present */
    CHECK_EQ_INT(sonicSamplesAvailable(s), 0);

    CHECK_EQ_MEM(got, in, 1000 * sizeof(short));
    for (int i = 0; i < 1000; i++) {
        CHECK_NEAR(fl[i], in[1000 + i] / 32767.0f, 1e-6);
        CHECK_EQ_INT(uc[i], (unsigned char)((in[2000 + i] >> 8) + 128));
    }
    sonicDestroyStream(s);
    free(in); free(got); free(fl); free(uc);
}

/* ------------------------------------------------------------------ speed */

static void check_speed_ratio(int sr, int ch, int quality, float speed, double tol)
{
    const int n = sr * 3;                      /* three seconds */
    double f[2] = { 220.0, 330.0 };
    short *in = pcm_sine(n, ch, sr, f, 2, 10000);
    Out o = run(in, n, sr, ch, speed, 1.0f, 1.0f, 1.0f, quality, 1000);
    double ratio = (double)n / o.frames;
    CHECK_MSG(fabs(ratio - speed) < tol * speed, "sr=%d ch=%d q=%d speed=%.2f: in=%d out=%d ratio=%.3f",
              sr, ch, quality, speed, n, o.frames, ratio);
    /* time-stretching must preserve pitch: zero-crossing frequency of every channel unchanged */
    for (int c = 0; c < ch; c++) {
        double got = pcm_freq(o.pcm, o.frames, ch, c, sr);
        CHECK_MSG(fabs(got - f[c]) < 0.03 * f[c], "sr=%d ch=%d speed=%.2f channel %d: %.1f Hz, want %.1f", sr, ch,
                  speed, c, got, f[c]);
    }
    free(in); free(o.pcm);
}

TEST(speeding_up_by_one_and_a_half_shortens_a_mono_sine_by_that_factor_keeping_pitch)
{
    check_speed_ratio(22050, 1, 0, 1.5f, 0.04);
}

TEST(speeding_up_by_two_shortens_a_mono_sine_by_that_factor_keeping_pitch)
{
    check_speed_ratio(22050, 1, 0, 2.0f, 0.04);
}

TEST(speeding_up_by_three_uses_the_partial_period_skip_and_keeps_pitch)
{
    check_speed_ratio(22050, 1, 0, 3.0f, 0.05);
}

TEST(speeding_up_by_six_still_reaches_the_target_duration)
{
    check_speed_ratio(16000, 1, 0, 6.0f, 0.08);
}

TEST(slowing_down_to_three_quarters_stretches_a_mono_sine_keeping_pitch)
{
    check_speed_ratio(22050, 1, 0, 0.75f, 0.04);
}

TEST(slowing_down_to_exactly_half_stretches_by_two)
{
    check_speed_ratio(22050, 1, 0, 0.5f, 0.04);
}

TEST(slowing_down_to_a_third_inserts_partial_periods_and_keeps_pitch)
{
    check_speed_ratio(22050, 1, 0, 0.3333f, 0.05);
}

TEST(stereo_44100_with_distinct_channel_frequencies_keeps_both_pitches_when_sped_up)
{
    check_speed_ratio(44100, 2, 0, 1.25f, 0.04);
}

TEST(stereo_44100_slowed_down_keeps_both_pitches)
{
    check_speed_ratio(44100, 2, 0, 0.8f, 0.04);
}

TEST(high_quality_mode_without_down_sampling_gives_the_same_stretch_ratio)
{
    check_speed_ratio(22050, 1, 1, 1.5f, 0.04);
    check_speed_ratio(22050, 2, 1, 0.75f, 0.04);
}

TEST(very_low_sample_rate_needs_no_down_sampling_in_default_quality)
{
    check_speed_ratio(4000, 1, 0, 1.5f, 0.06);   /* 4000 is not > SONIC_AMDF_FREQ: skip == 1 */
}

TEST(sample_rate_above_amdf_frequency_with_two_channels_down_samples_and_refines)
{
    check_speed_ratio(8000, 2, 0, 2.0f, 0.06);
}

TEST(speed_one_with_a_non_neutral_pitch_is_not_treated_as_pass_through)
{
    /* pitch 2.0 -> effective speed/pitch = 0.5: the stream must stretch and then resample */
    const int sr = 22050, n = sr * 2;
    short *in = pcm_sine1(n, sr, 300, 9000);
    Out o = run_default(in, n, sr, 1, 1.0f, 2.0f, 1.0f, 1.0f);
    CHECK_NEAR((double)o.frames / n, 1.0, 0.05);                  /* duration preserved */
    CHECK_NEAR(pcm_freq(o.pcm, o.frames, 1, 0, sr), 600.0, 600.0 * 0.03);  /* octave up */
    free(in); free(o.pcm);
}

/* ------------------------------------------------------------------ pitch, rate, volume */

TEST(pitch_two_doubles_the_frequency_without_changing_the_duration)
{
    const int sr = 22050, n = sr * 3;
    short *in = pcm_sine1(n, sr, 250, 9000);
    Out o = run_default(in, n, sr, 1, 1.0f, 2.0f, 1.0f, 1.0f);
    CHECK_NEAR((double)o.frames / n, 1.0, 0.04);
    CHECK_NEAR(pcm_freq(o.pcm, o.frames, 1, 0, sr), 500.0, 15.0);
    free(in); free(o.pcm);
}

TEST(pitch_one_half_halves_the_frequency_without_changing_the_duration)
{
    const int sr = 22050, n = sr * 3;
    short *in = pcm_sine1(n, sr, 400, 9000);
    Out o = run_default(in, n, sr, 1, 1.0f, 0.5f, 1.0f, 1.0f);
    CHECK_NEAR((double)o.frames / n, 1.0, 0.04);
    CHECK_NEAR(pcm_freq(o.pcm, o.frames, 1, 0, sr), 200.0, 6.0);
    free(in); free(o.pcm);
}

TEST(rate_two_halves_the_duration_and_doubles_the_frequency)
{
    const int sr = 22050, n = sr * 3;
    short *in = pcm_sine1(n, sr, 250, 9000);
    Out o = run_default(in, n, sr, 1, 1.0f, 1.0f, 2.0f, 1.0f);
    CHECK_NEAR((double)n / o.frames, 2.0, 0.06);
    CHECK_NEAR(pcm_freq(o.pcm, o.frames, 1, 0, sr), 500.0, 15.0);
    free(in); free(o.pcm);
}

TEST(rate_three_quarters_lengthens_and_lowers_the_pitch_in_stereo_at_44100)
{
    const int sr = 44100, n = sr * 2;
    double f[2] = { 300.0, 300.0 };
    short *in = pcm_sine(n, 2, sr, f, 2, 9000);
    Out o = run_default(in, n, sr, 2, 1.0f, 1.0f, 0.75f, 1.0f);
    CHECK_NEAR((double)n / o.frames, 0.75, 0.03);
    CHECK_NEAR(pcm_freq(o.pcm, o.frames, 2, 0, sr), 225.0, 7.0);
    CHECK_NEAR(pcm_freq(o.pcm, o.frames, 2, 1, sr), 225.0, 7.0);
    free(in); free(o.pcm);
}

TEST(rate_conversion_preserves_amplitude_of_a_low_frequency_sine)
{
    const int sr = 22050, n = sr * 2;
    short *in = pcm_sine1(n, sr, 200, 10000);
    Out o = run_default(in, n, sr, 1, 1.0f, 1.0f, 1.5f, 1.0f);
    double rms = pcm_rms(o.pcm, o.frames, 1, 0, 300);
    CHECK_NEAR(rms, 10000 / sqrt(2.0), 250.0);
    free(in); free(o.pcm);
}

TEST(volume_scales_amplitude_linearly)
{
    const int sr = 8000, n = sr * 2;
    short *in = pcm_sine1(n, sr, 300, 12000);
    Out half = run_default(in, n, sr, 1, 1.0f, 1.0f, 1.0f, 0.5f);
    Out dbl = run_default(in, n, sr, 1, 1.0f, 1.0f, 1.0f, 2.0f);
    CHECK_EQ_INT(half.frames, n);
    CHECK_EQ_INT(dbl.frames, n);
    for (int i = 0; i < n; i += 97) {
        CHECK_EQ_INT(half.pcm[i], (in[i] * 128) >> 8);       /* exact fixed-point 8.8 arithmetic */
        CHECK_EQ_INT(dbl.pcm[i], (in[i] * 512) >> 8);
    }
    free(in); free(half.pcm); free(dbl.pcm);
}

TEST(volume_above_unity_clips_at_the_16_bit_limits_instead_of_wrapping)
{
    const int sr = 8000, n = 4000;
    short *in = pcm_sine1(n, sr, 100, 30000);
    Out o = run_default(in, n, sr, 1, 1.0f, 1.0f, 1.0f, 4.0f);
    CHECK_EQ_INT(o.frames, n);
    int maxv = 0, minv = 0;
    for (int i = 0; i < n; i++) {
        if (o.pcm[i] > maxv) maxv = o.pcm[i];
        if (o.pcm[i] < minv) minv = o.pcm[i];
        if (in[i] > 9000) CHECK_EQ_INT(o.pcm[i], 32767);     /* positive overload saturates */
        if (in[i] < -9000) CHECK_EQ_INT(o.pcm[i], -32767);
    }
    CHECK_EQ_INT(maxv, 32767);
    CHECK_EQ_INT(minv, -32767);
    free(in); free(o.pcm);
}

TEST(volume_applies_after_time_stretching_to_every_returned_sample)
{
    const int sr = 22050, n = sr * 2;
    short *in = pcm_sine1(n, sr, 300, 16000);
    Out a = run_default(in, n, sr, 1, 1.5f, 1.0f, 1.0f, 1.0f);
    Out b = run_default(in, n, sr, 1, 1.5f, 1.0f, 1.0f, 0.25f);
    CHECK_EQ_INT(a.frames, b.frames);
    double ra = pcm_rms(a.pcm, a.frames, 1, 0, 2000), rb = pcm_rms(b.pcm, b.frames, 1, 0, 2000);
    CHECK_NEAR(rb / ra, 0.25, 0.01);
    free(in); free(a.pcm); free(b.pcm);
}

/* ------------------------------------------------------------------ energy / quality */

TEST(time_stretch_preserves_signal_energy_within_tolerance)
{
    const int sr = 22050, n = sr * 3;
    short *in = pcm_sine1(n, sr, 260, 10000);
    float speeds[] = { 0.6f, 0.9f, 1.3f, 1.8f, 2.5f };
    for (int k = 0; k < 5; k++) {
        Out o = run_default(in, n, sr, 1, speeds[k], 1.0f, 1.0f, 1.0f);
        double rms = pcm_rms(o.pcm, o.frames, 1, 0, 3000);
        CHECK_MSG(fabs(rms - 10000 / sqrt(2.0)) < 800, "speed %.2f: rms %.0f", speeds[k], rms);
        free(o.pcm);
    }
    free(in);
}

TEST(silence_in_gives_silence_out_at_any_speed)
{
    const int sr = 16000, n = sr * 2;
    short *in = calloc(n, sizeof(short));
    float speeds[] = { 0.4f, 0.8f, 1.5f, 3.0f };
    for (int k = 0; k < 4; k++) {
        Out o = run_default(in, n, sr, 1, speeds[k], 1.0f, 1.0f, 1.0f);
        CHECK_MSG(o.frames > 0, "speed %.2f produced no output", speeds[k]);
        CHECK_EQ_INT(pcm_peak(o.pcm, o.frames), 0);
        free(o.pcm);
    }
    free(in);
}

TEST(speech_like_signal_with_changing_period_and_silence_gaps_is_stretched_correctly)
{
    /* exercises prevPeriodBetter(): abrupt ends of voiced bursts followed by silence and a different pitch */
    const int sr = 22050, n = sr * 4;
    short *in = calloc(n, sizeof(short));
    srand(12345);
    for (int i = 0; i < n; i++) {
        int seg = (i / 4000) % 4;
        double f = seg == 0 ? 150 : seg == 1 ? 0 : seg == 2 ? 310 : 95;
        in[i] = f ? (short)(8000 * sin(2 * M_PI * f * i / sr) + (rand() % 400) - 200) : (short)((rand() % 40) - 20);
    }
    float speeds[] = { 1.4f, 0.7f, 2.2f, 0.4f };
    for (int k = 0; k < 4; k++) {
        Out o = run_default(in, n, sr, 1, speeds[k], 1.0f, 1.0f, 1.0f);
        double ratio = (double)n / o.frames;
        CHECK_MSG(fabs(ratio - speeds[k]) < 0.08 * speeds[k], "speed %.2f ratio %.3f", speeds[k], ratio);
        free(o.pcm);
    }
    free(in);
}

TEST(white_noise_input_is_stretched_to_the_requested_length)
{
    const int sr = 11025, n = sr * 3;
    short *in = malloc(n * sizeof(short));
    srand(7);
    for (int i = 0; i < n; i++) in[i] = (short)((rand() % 20000) - 10000);
    float speeds[] = { 1.6f, 0.6f };
    for (int k = 0; k < 2; k++) {
        Out o = run_default(in, n, sr, 1, speeds[k], 1.0f, 1.0f, 1.0f);
        CHECK_MSG(fabs((double)n / o.frames - speeds[k]) < 0.08 * speeds[k], "speed %.2f: %d -> %d", speeds[k], n, o.frames);
        free(o.pcm);
    }
    free(in);
}

TEST(full_scale_square_wave_through_the_resampler_clips_instead_of_wrapping)
{
    /* The windowed-sinc interpolator overshoots on a hard edge; the overflow guard must saturate. */
    const int sr = 8000, n = 8000;
    short *in = malloc(n * sizeof(short));
    for (int i = 0; i < n; i++) in[i] = ((i / 7) & 1) ? 32767 : -32768;
    float rates[] = { 1.37f, 0.83f, 1.9f };
    int saw_max = 0, saw_min = 0;
    for (int k = 0; k < 3; k++) {
        Out o = run_default(in, n, sr, 1, 1.0f, 1.0f, rates[k], 1.0f);
        CHECK(o.frames > n / 3);
        for (int i = 0; i < o.frames; i++) {
            if (o.pcm[i] == SHRT_MAX) saw_max = 1;
            if (o.pcm[i] == SHRT_MIN) saw_min = 1;
        }
        free(o.pcm);
    }
    CHECK(saw_max);
    CHECK(saw_min);
    free(in);
}

/* ------------------------------------------------------------------ streaming behaviour */

TEST(output_length_does_not_depend_on_how_the_input_is_chunked)
{
    const int sr = 22050, n = sr * 2;
    short *in = pcm_sine1(n, sr, 300, 9000);
    int chunks[] = { 100, 777, 4096, 50000 };
    int base = -1;
    for (int k = 0; k < 4; k++) {
        Out o = run(in, n, sr, 1, 1.5f, 1.0f, 1.0f, 1.0f, 0, chunks[k]);
        if (base < 0) base = o.frames;
        CHECK_MSG(abs(o.frames - base) < sr / 20, "chunk %d: %d frames vs %d", chunks[k], o.frames, base);
        free(o.pcm);
    }
    free(in);
}

TEST(changing_speed_in_the_middle_of_a_stream_blends_the_play_time_budget)
{
    const int sr = 22050;
    const int half = sr * 2;
    short *in = pcm_sine1(half, sr, 300, 9000);
    short *out = malloc((size_t)sr * 12 * sizeof(short));
    int total = 0;
    sonicStream s = sonicCreateStream(sr, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, 2.0f);
    for (int pos = 0; pos < half; pos += 900) {
        REQUIRE(sonicWriteShortToStream(s, in + pos, 900) == 1);
        total += sonicReadShortFromStream(s, out + total, 1 << 20);
    }
    int first = total;
    CHECK_NEAR((double)half / first, 2.0, 0.2);
    sonicSetSpeed(s, 0.5f);                    /* sudden switch; input still buffered keeps its old budget */
    for (int pos = 0; pos < half; pos += 900) {
        REQUIRE(sonicWriteShortToStream(s, in + pos, 900) == 1);
        total += sonicReadShortFromStream(s, out + total, 1 << 20);
    }
    REQUIRE(sonicFlushStream(s) == 1);
    total += sonicReadShortFromStream(s, out + total, 1 << 20);
    CHECK_NEAR((double)(total - first) / half, 2.0, 0.2);
    CHECK_NEAR(sonicProcessingSpeed(s), 0.5, 0.6);   /* last burst mixes retained input: not above old speed */
    sonicDestroyStream(s);
    free(in); free(out);
}

TEST(processing_speed_reports_the_budgeted_speed_of_the_last_burst)
{
    const int sr = 16000;
    short *in = pcm_sine1(sr, sr, 300, 9000);
    sonicStream s = sonicCreateStream(sr, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, 1.6f);
    REQUIRE(sonicWriteShortToStream(s, in, sr) == 1);
    CHECK_NEAR(sonicProcessingSpeed(s), 1.6f, 0.01);
    CHECK(sonicInputSamplesBuffered(s) >= 0);
    CHECK(sonicInputSamplesBuffered(s) < sr);
    sonicDestroyStream(s);
    free(in);
}

TEST(below_the_look_ahead_threshold_input_is_held_back_until_flush)
{
    const int sr = 22050, need = 2 * (sr / SONIC_MIN_PITCH);
    short *in = pcm_sine1(need - 10, sr, 300, 9000);
    sonicStream s = sonicCreateStream(sr, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, 2.0f);
    REQUIRE(sonicWriteShortToStream(s, in, need - 10) == 1);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 0);                   /* changeSpeed() wants maxRequired samples */
    CHECK_EQ_INT(sonicInputSamplesBuffered(s), need - 10);
    REQUIRE(sonicFlushStream(s) == 1);
    CHECK_NEAR(sonicSamplesAvailable(s), (need - 10) / 2, 4);   /* flush yields exactly the expected tail */
    CHECK_EQ_INT(sonicInputSamplesBuffered(s), 0);
    sonicDestroyStream(s);
    free(in);
}

TEST(flush_on_an_empty_stream_succeeds_and_yields_nothing)
{
    sonicStream s = sonicCreateStream(22050, 2);
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, 1.5f);
    CHECK_EQ_INT(sonicFlushStream(s), 1);
    CHECK_EQ_INT(sonicSamplesAvailable(s), 0);
    CHECK_EQ_INT(sonicInputSamplesBuffered(s), 0);
    sonicDestroyStream(s);
}

TEST(flush_drains_the_pitch_buffer_of_a_rate_converted_stream)
{
    const int sr = 22050, n = 6000;
    short *in = pcm_sine1(n, sr, 300, 9000);
    sonicStream s = sonicCreateStream(sr, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetRate(s, 1.25f);
    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    int before = sonicSamplesAvailable(s);
    REQUIRE(sonicFlushStream(s) == 1);
    int after = sonicSamplesAvailable(s);
    CHECK(after > before);                                       /* the held-back FIR tail was released */
    CHECK_NEAR((double)n / after, 1.25, 0.02);
    sonicDestroyStream(s);
    free(in);
}

TEST(large_single_write_grows_the_input_and_output_buffers)
{
    const int sr = 8000, n = sr * 40;           /* 320k frames: far beyond the initial 25% headroom */
    short *in = pcm_sine1(n, sr, 200, 8000);
    sonicStream s = sonicCreateStream(sr, 1);
    short *out = malloc((size_t)n * 3 * sizeof(short));
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, 0.5f);                     /* output twice as large as input: output must grow too */
    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    REQUIRE(sonicFlushStream(s) == 1);
    int got = sonicReadShortFromStream(s, out, n * 3);
    CHECK_NEAR((double)got / n, 2.0, 0.08);
    CHECK_NEAR(pcm_freq(out, got, 1, 0, sr), 200.0, 6.0);
    sonicDestroyStream(s);
    free(in); free(out);
}

TEST(long_slow_down_in_small_writes_grows_the_pitch_buffer_for_rate_conversion)
{
    const int sr = 8000, n = sr * 30;
    short *in = pcm_sine1(n, sr, 200, 8000);
    sonicStream s = sonicCreateStream(sr, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetSpeed(s, 0.25f);
    sonicSetRate(s, 1.5f);
    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    REQUIRE(sonicFlushStream(s) == 1);
    CHECK_NEAR((double)sonicSamplesAvailable(s) / n, 4.0 / 1.5, 0.2);
    sonicDestroyStream(s);
    free(in);
}

/* ------------------------------------------------------------------ reconfiguration */

TEST(set_sample_rate_reallocates_and_the_stream_keeps_working)
{
    sonicStream s = sonicCreateStream(8000, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetSampleRate(s, 22050);
    CHECK_EQ_INT(sonicGetSampleRate(s), 22050);
    sonicSetSampleRate(s, 1);                    /* clamped */
    CHECK_EQ_INT(sonicGetSampleRate(s), SONIC_MIN_SAMPLE_RATE);
    sonicSetSampleRate(s, 16000);
    CHECK_EQ_INT(sonicGetSampleRate(s), 16000);

    const int n = 16000 * 2;
    short *in = pcm_sine1(n, 16000, 300, 9000);
    short *out = malloc(n * sizeof(short));
    sonicSetSpeed(s, 2.0f);
    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    REQUIRE(sonicFlushStream(s) == 1);
    int got = sonicReadShortFromStream(s, out, n);
    CHECK_NEAR((double)n / got, 2.0, 0.1);
    CHECK_NEAR(pcm_freq(out, got, 1, 0, 16000), 300.0, 9.0);
    sonicDestroyStream(s);
    free(in); free(out);
}

TEST(set_num_channels_reallocates_and_the_stream_keeps_working)
{
    sonicStream s = sonicCreateStream(8000, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetNumChannels(s, 2);
    CHECK_EQ_INT(sonicGetNumChannels(s), 2);
    sonicSetNumChannels(s, 100);
    CHECK_EQ_INT(sonicGetNumChannels(s), SONIC_MAX_CHANNELS);
    sonicSetNumChannels(s, 0);
    CHECK_EQ_INT(sonicGetNumChannels(s), 1);
    sonicSetNumChannels(s, 2);

    const int n = 8000;
    double f[2] = { 200, 400 };
    short *in = pcm_sine(n, 2, 8000, f, 2, 9000);
    short *out = malloc(n * 2 * sizeof(short));
    REQUIRE(sonicWriteShortToStream(s, in, n) == 1);
    CHECK_EQ_INT(sonicReadShortFromStream(s, out, n), n);
    CHECK_EQ_MEM(in, out, n * 2 * sizeof(short));
    sonicDestroyStream(s);
    free(in); free(out);
}

TEST(set_rate_resets_the_resampler_phase_so_equal_runs_give_equal_output)
{
    const int sr = 22050, n = 8000;
    short *in = pcm_sine1(n, sr, 300, 9000);
    short *a = malloc(n * 2 * sizeof(short)), *b = malloc(n * 2 * sizeof(short));
    sonicStream s = sonicCreateStream(sr, 1);
    REQUIRE_NOT_NULL(s);
    sonicSetRate(s, 1.3f);
    sonicWriteShortToStream(s, in, n);
    sonicFlushStream(s);
    int na = sonicReadShortFromStream(s, a, n * 2);
    sonicSetRate(s, 1.3f);                       /* resets old/newRatePosition */
    sonicWriteShortToStream(s, in, n);
    sonicFlushStream(s);
    int nb = sonicReadShortFromStream(s, b, n * 2);
    CHECK_EQ_INT(na, nb);
    CHECK_EQ_MEM(a, b, (size_t)na * sizeof(short));
    sonicDestroyStream(s);
    free(in); free(a); free(b);
}

/* ------------------------------------------------------------------ one-shot helpers */

TEST(change_short_speed_processes_a_whole_buffer_in_place)
{
    const int sr = 22050, n = sr * 2;
    short *buf = malloc((size_t)n * sizeof(short));
    short *src = pcm_sine1(n, sr, 300, 9000);
    memcpy(buf, src, n * sizeof(short));
    int got = sonicChangeShortSpeed(buf, n, 2.0f, 1.0f, 1.0f, 1.0f, 0, sr, 1);
    CHECK_NEAR((double)n / got, 2.0, 0.1);
    CHECK_NEAR(pcm_freq(buf, got, 1, 0, sr), 300.0, 9.0);
    free(buf); free(src);
}

TEST(change_short_speed_applies_pitch_rate_and_volume_together)
{
    const int sr = 22050, n = sr * 2;
    short *buf = pcm_sine1(n, sr, 200, 20000);
    int got = sonicChangeShortSpeed(buf, n, 1.0f, 1.0f, 2.0f, 0.5f, 0, sr, 1);
    CHECK_NEAR((double)n / got, 2.0, 0.1);
    CHECK_NEAR(pcm_freq(buf, got, 1, 0, sr), 400.0, 12.0);
    CHECK_NEAR(pcm_rms(buf, got, 1, 0, 400), 10000 / sqrt(2.0), 500.0);
    free(buf);
}

TEST(change_float_speed_processes_a_whole_buffer_in_place)
{
    const int sr = 22050, n = sr * 2;
    /* sonicChange*Speed() reads the result back into the caller's buffer and takes no capacity argument,
     * so the buffer must hold the OUTPUT: half speed makes it ~2x longer than the input. */
    float *buf = calloc((size_t)n * 3, sizeof(float));
    for (int i = 0; i < n; i++) buf[i] = 0.4f * (float)sin(2 * M_PI * 300.0 * i / sr);
    int got = sonicChangeFloatSpeed(buf, n, 0.5f, 1.0f, 1.0f, 1.0f, 0, sr, 1);
    CHECK_NEAR((double)got / n, 2.0, 0.1);
    short *tmp = malloc(got * sizeof(short));
    float peak = 0;
    for (int i = 0; i < got; i++) { tmp[i] = (short)(buf[i] * 32767); if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]); }
    CHECK_NEAR(pcm_freq(tmp, got, 1, 0, sr), 300.0, 9.0);
    CHECK_NEAR(peak, 0.4, 0.03);
    free(buf); free(tmp);
}

/* ------------------------------------------------------------------ allocation failure (real RLIMIT_AS) */

static long vm_bytes(void)
{
    long pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld", &pages) != 1) pages = 0; fclose(f); }
    return pages * sysconf(_SC_PAGESIZE);
}

/* Run body(arg) in a forked child whose address space is capped at (current + extra) bytes. The body's
 * return value (0..100) is the child's exit status. */
static int with_limit(long extra, int (*body)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = vm_bytes() + extra;
        setrlimit(RLIMIT_AS, &rl);
        int rc = body();
        __gcov_dump();
        _exit(rc);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200;
}

static int body_create_max(void)
{
    sonicStream s = sonicCreateStream(SONIC_MAX_SAMPLE_RATE, SONIC_MAX_CHANNELS);
    if (!s) return 1;
    sonicDestroyStream(s);
    return 0;
}

TEST(stream_creation_returns_null_when_any_internal_buffer_cannot_be_allocated)
{
    /* Each of the four buffers is ~1.2 MB at the maximum format; sweeping the cap makes each
     * calloc in turn fail first. */
    int nulls = 0, oks = 0;
    for (long extra = 0; extra < 6L * 1024 * 1024; extra += 200 * 1024) {
        int rc = with_limit(extra, body_create_max);
        CHECK_MSG(rc == 0 || rc == 1, "child died with %d at extra=%ld", rc, extra);
        if (rc == 1) nulls++; else oks++;
    }
    CHECK(nulls > 10);
    CHECK(oks > 0);
}

static int body_big_write(void)
{
    sonicStream s = sonicCreateStream(8000, 1);
    if (!s) return 9;
    static short tiny[16];
    int rc = sonicWriteShortToStream(s, tiny, 100 * 1000 * 1000);   /* 200 MB: realloc must fail first */
    return rc == 0 ? 1 : 0;
}

TEST(writing_more_than_the_address_space_allows_reports_failure_instead_of_crashing)
{
    CHECK_EQ_INT(with_limit(40L * 1024 * 1024, body_big_write), 1);
}

static int body_big_write_float(void)
{
    sonicStream s = sonicCreateStream(8000, 1);
    if (!s) return 9;
    static float tiny[16];
    return sonicWriteFloatToStream(s, tiny, 100 * 1000 * 1000) == 0 ? 1 : 0;
}

static int body_big_write_uchar(void)
{
    sonicStream s = sonicCreateStream(8000, 1);
    if (!s) return 9;
    static unsigned char tiny[16];
    return sonicWriteUnsignedCharToStream(s, tiny, 100 * 1000 * 1000) == 0 ? 1 : 0;
}

TEST(float_and_unsigned_char_writes_also_report_allocation_failure)
{
    CHECK_EQ_INT(with_limit(40L * 1024 * 1024, body_big_write_float), 1);
    CHECK_EQ_INT(with_limit(40L * 1024 * 1024, body_big_write_uchar), 1);
}

/* Large write that fits the input buffer but whose output (slow-down / rate) does not. */
static int body_output_pressure(void)
{
    const int n = 1500 * 1000;
    sonicStream s = sonicCreateStream(8000, 1);
    short *in = calloc(n, sizeof(short));
    if (!s || !in) return 9;
    for (int i = 0; i < n; i++) in[i] = (short)(8000 * sin(2 * M_PI * 200.0 * i / 8000));
    sonicSetSpeed(s, 0.3f);
    sonicSetRate(s, 1.1f);
    return sonicWriteShortToStream(s, in, n) ? 0 : 1;
}

TEST(output_buffer_growth_failure_is_reported_for_slow_down_and_rate_change)
{
    int fails = 0;
    for (long extra = 3L * 1024 * 1024; extra < 24L * 1024 * 1024; extra += 1024 * 1024) {
        int rc = with_limit(extra, body_output_pressure);
        CHECK_MSG(rc == 0 || rc == 1, "child status %d at extra=%ld", rc, extra);
        if (rc == 1) fails++;
    }
    CHECK(fails > 3);
}

/*
 * UNREACHABLE on host (sonic.c, 18 of 580 lines uncovered; all are failure returns or dead code):
 *
 *   403-404, 427, 583, 595, 708, 715, 929, 1005, 1008, 1015, 1050, 1095, 1121, 1182, 1187
 *       `return 0` / `return NULL` after a malloc/realloc inside sonicCreateStream, the enlarge*Buffer
 *       helpers and the process* routines. The allocation-failure tests above drive real out-of-memory
 *       through RLIMIT_AS, but which of ~15 allocation sites fails first depends on the allocator and
 *       cannot be steered to each one without replacing malloc (i.e. faking the environment).
 *   818-819
 *       prevPeriodBetter(): the `preferNewPeriod == 0` branch. Its only caller (findPitchPeriod at
 *       sonic.c:1127) always passes 1, so this branch is dead code in production.
 */
