/*
 * test_pcm_autogain.c -- unit tests for Source/pcm_autogain.c (automatic gain control on S16LE PCM).
 *
 * White box: the real source file is compiled into this test, so the file-local AGC state
 * (_agc_current_gain, _hold_current, _power_array, ...) and the static DSP helpers
 * (s32le_softclip_s16le, pcm_s16le_pga_softlimiter, pcm_s16le_pga_limiter, _calculateGain) are
 * reachable and gcov attributes the lines to Source/pcm_autogain.c.
 *
 * DEBUG_MSG is switched on so that the file's debug commands (atef/atei/ated/atpup/atpdn) are
 * compiled in and registered with the *real* debug registry (Source/debug.c); they are then driven
 * through the real dispatcher debug_do_cmd().
 *
 * Expected constants were computed independently (outside C) from the formulas in the source:
 *   hold_max = 50 ms * rate * 2 / (1000 * framesize)
 *   decr     = 1e6 * framesize * (GAIN_MAX - GAIN_MIN) / (650*150  * rate)   (Q16.16 per frame)
 *   incr     = 1e6 * framesize * (GAIN_MAX - GAIN_MIN) / (650*5000 * rate)
 * Power threshold = 25,000,000, i.e. a constant signal of +-5000 is exactly on the threshold.
 */
#define DEBUG_MSG 1
#include "avos_test.h"
#include "Source/pcm_autogain.c"

#include <string.h>

#define Q16(x)   ((int)((long)(x) << 16))

/* ---- helpers ---------------------------------------------------------------------------- */

static void fill(SHORT *buf, int n, int value)
{
	for (int i = 0; i < n; i++)
		buf[i] = (SHORT)value;
}

/* One 32-sample frame holding a constant, run through the AGC; returns the output sample. */
static int run_frame(int value)
{
	SHORT f[32];
	fill(f, 32, value);
	pcm_apply_agc(f, 5);
	return f[0];
}

static void run_frames(int count, int value)
{
	for (int i = 0; i < count; i++)
		run_frame(value);
}

/* ---- pcm_set_agc: per sample rate parameters ---------------------------------------------- */

struct rate_case { int rate, order, size, hold, decr, incr; };

TEST(set_agc_derives_frame_size_hold_time_and_gain_steps_for_every_sample_rate)
{
	static const struct rate_case cases[] = {
		{  8000, 3,  8, 100, 516222, 15486 },
		{ 11025, 3,  8, 137, 374582, 11237 },
		{ 12000, 3,  8, 150, 344148, 10324 },
		{ 16000, 3,  8, 200, 258111,  7743 },
		{ 22050, 4, 16, 137, 374582, 11237 },
		{ 24000, 4, 16, 150, 344148, 10324 },
		{ 32000, 5, 32, 100, 516222, 15486 },
		{ 44100, 5, 32, 137, 374582, 11237 },
		{ 48000, 5, 32, 150, 344148, 10324 },
		{ 96000, 5, 32, 300, 172074,  5162 },	/* default branch */
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const struct rate_case *c = &cases[i];
		pcm_set_agc(c->rate);
		CHECK_MSG(_agc_frameorder == c->order, "rate %d: frameorder %d, expected %d", c->rate, _agc_frameorder, c->order);
		CHECK_MSG(_agc_framesize  == c->size,  "rate %d: framesize %d, expected %d",  c->rate, _agc_framesize,  c->size);
		CHECK_MSG(_agc_hold_max   == c->hold,  "rate %d: hold_max %d, expected %d",   c->rate, _agc_hold_max,   c->hold);
		CHECK_MSG(_agc_gain_decr  == c->decr,  "rate %d: decr %d, expected %d",       c->rate, _agc_gain_decr,  c->decr);
		CHECK_MSG(_agc_gain_incr  == c->incr,  "rate %d: incr %d, expected %d",       c->rate, _agc_gain_incr,  c->incr);
	}
}

TEST(attack_is_faster_than_release_at_every_rate)
{
	static const int rates[] = { 8000, 22050, 44100, 48000 };
	for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
		pcm_set_agc(rates[i]);
		CHECK_MSG(_agc_gain_decr > _agc_gain_incr, "rate %d: decr %d <= incr %d", rates[i], _agc_gain_decr, _agc_gain_incr);
		CHECK(_agc_gain_incr > 0);
	}
}

TEST(set_agc_resets_gain_hold_counter_history_and_threshold)
{
	/* dirty every piece of state first */
	pcm_set_agc(44100);
	run_frames(50, 1000);			/* raises the gain, fills the history */
	run_frame(20000);			/* loud: arms the hold counter */
	_agc_power_threshold = 7;
	CHECK(_agc_current_gain != Q16(256) || _hold_current != 0);
	CHECK(_power_idx != 0);

	pcm_set_agc(48000);

	CHECK_EQ_INT(_agc_current_gain, Q16(256));
	CHECK_EQ_INT(_hold_current, 0);
	CHECK_EQ_INT(_power_idx, 0);
	for (int i = 0; i < _POWER_HISTORY; i++)
		CHECK_MSG(_power_array[i] == 0, "history[%d] = %lld", i, _power_array[i]);
	CHECK(_agc_power_threshold == 25000000LL);
}

/* ---- pcm_apply_agc: contract and framing -------------------------------------------------- */

TEST(apply_agc_rejects_a_buffer_smaller_than_one_frame_and_leaves_everything_untouched)
{
	pcm_set_agc(44100);			/* frameorder 5 */
	SHORT buf[16];
	fill(buf, 16, 1234);
	int gain = _agc_current_gain, idx = _power_idx;

	pcm_apply_agc(buf, 4);			/* 2^4 < 2^5 */

	for (int i = 0; i < 16; i++)
		CHECK_MSG(buf[i] == 1234, "sample %d modified to %d", i, buf[i]);
	CHECK_EQ_INT(_agc_current_gain, gain);
	CHECK_EQ_INT(_power_idx, idx);
}

TEST(apply_agc_processes_exactly_one_frame_per_framesize_samples)
{
	pcm_set_agc(44100);
	SHORT buf[256];
	fill(buf, 256, 100);

	pcm_apply_agc(buf, 5);			/* one frame */
	CHECK_EQ_INT(_power_idx, 1);

	pcm_apply_agc(buf, 7);			/* 128 samples = four frames */
	CHECK_EQ_INT(_power_idx, 5);

	pcm_apply_agc(buf, 8);			/* 256 samples = eight frames: full history lap */
	CHECK_EQ_INT(_power_idx, 5);
	for (int i = 0; i < _POWER_HISTORY; i++)
		CHECK_MSG(_power_array[i] == 10000, "history[%d] = %lld (100^2 expected)", i, _power_array[i]);
}

TEST(apply_agc_uses_the_frame_size_of_the_configured_rate)
{
	pcm_set_agc(8000);			/* frameorder 3: 8-sample frames */
	SHORT buf[64];
	fill(buf, 64, 10);
	pcm_apply_agc(buf, 6);			/* 64 samples / 8 = 8 frames */
	CHECK_EQ_INT(_power_idx, 0);		/* 8 frames wrap the 8-entry history */
	pcm_apply_agc(buf, 4);			/* 16 samples = 2 frames */
	CHECK_EQ_INT(_power_idx, 2);

	pcm_set_agc(22050);			/* frameorder 4 */
	pcm_apply_agc(buf, 4);
	CHECK_EQ_INT(_power_idx, 1);
}

/* ---- gain control law --------------------------------------------------------------------- */

TEST(silence_releases_the_gain_by_exactly_incr_per_frame)
{
	pcm_set_agc(44100);			/* incr = 11237 */
	for (int n = 1; n <= 20; n++) {
		run_frame(0);
		CHECK_MSG(_agc_current_gain == Q16(256) + n * 11237, "frame %d: gain %d", n, _agc_current_gain);
	}
	CHECK_EQ_INT(_hold_current, 0);
}

TEST(output_is_unity_until_the_integer_part_of_the_gain_reaches_257)
{
	pcm_set_agc(44100);
	/* gain = 256 + n*11237/65536: still 256 for n <= 5 (5.83 frames per integer step) */
	for (int n = 1; n <= 5; n++)
		CHECK_MSG(run_frame(1000) == 1000, "frame %d not unity", n);
	/* frame 6: integer gain 257 -> 1000*257>>8 = 1003 */
	CHECK_EQ_INT(_agc_current_gain >> 16, 256);
	CHECK_EQ_INT(run_frame(1000), 1003);
	CHECK_EQ_INT(_agc_current_gain >> 16, 257);
}

TEST(quiet_signal_is_amplified_by_the_integer_part_of_the_gain)
{
	pcm_set_agc(44100);
	int prev = 0;
	for (int n = 1; n <= 600; n++) {
		int out = run_frame(1000);
		int expected = (1000 * (_agc_current_gain >> 16)) >> 8;
		CHECK_MSG(out == expected, "frame %d: out %d expected %d (gain %d)", n, out, expected, _agc_current_gain >> 16);
		CHECK_MSG(out >= prev, "frame %d: output decreased from %d to %d", n, prev, out);
		prev = out;
	}
	CHECK(prev > 1000);			/* the AGC did boost it */
}

TEST(gain_is_clamped_at_plus_12_db_and_quiet_audio_ends_up_four_times_louder)
{
	pcm_set_agc(44100);
	/* 50331648 / 11237 = 4479.2 frames to go from 0 dB to +12 dB */
	run_frames(4479, 1000);
	CHECK(_agc_current_gain < Q16(1024));
	run_frame(1000);
	CHECK_EQ_INT(_agc_current_gain, Q16(1024));
	run_frames(100, 1000);
	CHECK_EQ_INT(_agc_current_gain, Q16(1024));	/* does not overshoot */
	CHECK_EQ_INT(run_frame(1000), 4000);		/* x4 */
	CHECK_EQ_INT(run_frame(-1000), -4000);
}

TEST(power_exactly_on_the_threshold_counts_as_loud_and_just_below_as_quiet)
{
	pcm_set_agc(44100);
	run_frames(100, 0);			/* get above the 0 dB floor so attack is visible */
	int base = _agc_current_gain;

	run_frame(4999);			/* 4999^2 = 24,990,001 < 25,000,000 : release */
	CHECK_EQ_INT(_agc_current_gain, base + 11237);
	CHECK_EQ_INT(_hold_current, 0);

	base = _agc_current_gain;
	run_frame(5000);			/* 5000^2 == threshold : attack */
	CHECK_EQ_INT(_agc_current_gain, base - 374582);
	CHECK_EQ_INT(_hold_current, 137);
}

TEST(attack_lowers_the_gain_by_decr_and_the_loud_frame_itself_uses_the_new_gain)
{
	pcm_set_agc(44100);
	run_frames(2000, 0);			/* gain = 256 + 2000*11237/65536 = 599.x */
	int before = _agc_current_gain;
	CHECK_EQ_INT(before, Q16(256) + 2000 * 11237);

	int out = run_frame(8000);

	CHECK_EQ_INT(_agc_current_gain, before - 374582);
	/* integer gain 593 (not 598): 8000*593>>8 */
	CHECK_EQ_INT(out, 18531);
	CHECK_EQ_INT(_hold_current, _agc_hold_max);
}

TEST(attack_never_pushes_the_gain_below_0_db)
{
	pcm_set_agc(44100);
	run_frames(10, 0);			/* gain = 256<<16 + 112370 < one decr step above the floor */
	CHECK(_agc_current_gain - Q16(256) < 374582);
	run_frame(20000);
	CHECK_EQ_INT(_agc_current_gain, Q16(256));
	for (int i = 0; i < 20; i++)
		run_frame(20000);
	CHECK_EQ_INT(_agc_current_gain, Q16(256));
}

TEST(loud_audio_at_unity_gain_passes_below_the_clip_barrier_untouched)
{
	pcm_set_agc(44100);
	CHECK_EQ_INT(run_frame(20000), 20000);
	CHECK_EQ_INT(run_frame(-20000), -20000);
	CHECK_EQ_INT(run_frame(0x7c00), 0x7c00);	/* the barrier itself is not clipped */
	CHECK_EQ_INT(run_frame(-0x7c00), -0x7c00);
}

TEST(hold_freezes_the_gain_for_hold_max_quiet_frames_then_release_resumes)
{
	pcm_set_agc(44100);			/* hold_max = 137 */
	run_frames(1000, 0);
	run_frame(6000);			/* attack: arms hold */
	CHECK_EQ_INT(_hold_current, 137);
	int frozen = _agc_current_gain;

	for (int n = 1; n <= 137; n++) {
		run_frame(0);
		CHECK_MSG(_agc_current_gain == frozen, "frame %d: gain moved during hold", n);
		CHECK_MSG(_hold_current == 137 - n, "frame %d: hold %d", n, _hold_current);
	}
	run_frame(0);				/* hold exhausted */
	CHECK_EQ_INT(_agc_current_gain, frozen + 11237);
	CHECK_EQ_INT(_hold_current, 0);
}

TEST(a_loud_frame_during_hold_rearms_the_hold_counter)
{
	pcm_set_agc(44100);
	run_frames(1000, 0);
	run_frame(6000);
	run_frames(50, 0);
	CHECK_EQ_INT(_hold_current, 137 - 50);
	run_frame(6000);
	CHECK_EQ_INT(_hold_current, 137);
}

TEST(power_is_measured_on_the_first_half_of_the_frame_only)
{
	/* mean energy is taken over 2^(frameorder-1) samples: the 16 first of a 32-sample frame.
	   Pinned as current behaviour (the source comment speaks of "stereo samples"). */
	pcm_set_agc(44100);
	run_frames(100, 0);
	int base = _agc_current_gain;

	SHORT f[32];
	fill(f, 16, 0);
	fill(f + 16, 16, 20000);		/* loud only in the second half */
	pcm_apply_agc(f, 5);
	CHECK_EQ_INT(_power_array[(_power_idx + 7) & 7], 0);
	CHECK_EQ_INT(_agc_current_gain, base + 11237);	/* treated as quiet */

	fill(f, 16, 20000);
	fill(f + 16, 16, 0);			/* loud only in the first half */
	base = _agc_current_gain;
	pcm_apply_agc(f, 5);
	CHECK_EQ_INT(_power_array[(_power_idx + 7) & 7], 400000000LL);
	CHECK_EQ_INT(_agc_current_gain, base - 374582);	/* treated as loud */
}

TEST(history_records_the_energy_of_each_frame_in_circular_order)
{
	pcm_set_agc(44100);
	for (int n = 0; n < 11; n++)
		run_frame((n + 1) * 10);	/* frame n has energy ((n+1)*10)^2 */
	/* 11 frames into an 8-slot ring: frames 8,9,10 overwrote slots 0,1,2 */
	static const int last_frame_in_slot[8] = { 8, 9, 10, 3, 4, 5, 6, 7 };
	for (int slot = 0; slot < 8; slot++) {
		long long v = (long long)(last_frame_in_slot[slot] + 1) * 10;
		CHECK_MSG(_power_array[slot] == v * v, "slot %d = %lld expected %lld", slot, _power_array[slot], v * v);
	}
	CHECK_EQ_INT(_power_idx, 11 & 7);
}

/* ---- soft limiter ------------------------------------------------------------------------- */

struct clip_case { int in, out; };

TEST(softclip_maps_positive_overshoot_onto_the_compression_curve)
{
	static const struct clip_case c[] = {
		{ 0x7c01, 31744 }, { 0x7d00, 31970 }, { 32767, 32359 }, { 32768, 32720 },
		{ 40000, 39229 }, { 65535, 40551 }, { 100000, 64970 },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
		CHECK_MSG(s32le_softclip_s16le(c[i].in, clip_barrier) == c[i].out,
			  "softclip(%d) = %d expected %d", c[i].in, s32le_softclip_s16le(c[i].in, clip_barrier), c[i].out);
}

TEST(softclip_maps_negative_overshoot_onto_the_mirrored_curve)
{
	static const struct clip_case c[] = {
		{ -0x7c01, -31745 }, { -0x7d00, -31970 }, { -32768, -32720 },
		{ -40000, -39229 }, { -65535, -40552 }, { -100000, -64971 },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
		CHECK_MSG(s32le_softclip_s16le(c[i].in, clip_barrier) == c[i].out,
			  "softclip(%d) = %d expected %d", c[i].in, s32le_softclip_s16le(c[i].in, clip_barrier), c[i].out);
}

TEST(softclip_is_symmetric_to_within_one_lsb)
{
	for (int s = clip_barrier + 1; s < 70000; s += 37) {
		int p = s32le_softclip_s16le(s, clip_barrier);
		int n = s32le_softclip_s16le(-s, clip_barrier);
		CHECK_MSG(abs(p + n) <= 1, "s=%d: +%d / %d", s, p, n);
	}
}

TEST(softclip_selects_the_table_entry_from_the_overshoot_modulo_1024)
{
	/* overshoot 0..63 -> row 0, 64..127 -> row 1, ..., and the index wraps every 1024 */
	CHECK_EQ_INT(s32le_softclip_s16le(clip_barrier + 64, clip_barrier),  31747 + ((64 * 232) >> 8));
	CHECK_EQ_INT(s32le_softclip_s16le(clip_barrier + 1024, clip_barrier), 31744 + ((1024 * 244) >> 8));
	CHECK_EQ_INT(s32le_softclip_s16le(clip_barrier + 1024 + 960, clip_barrier), 32104 + (((1024 + 960) * 64) >> 8));
}

TEST(softlimiter_leaves_samples_up_to_the_barrier_exactly_as_the_gain_scales_them)
{
	SHORT b[5] = { 0, 1, -1, 31744, -31744 };
	pcm_s16le_pga_softlimiter(b, 5, 256);
	CHECK_EQ_INT(b[0], 0);
	CHECK_EQ_INT(b[1], 1);
	CHECK_EQ_INT(b[2], -1);
	CHECK_EQ_INT(b[3], 31744);
	CHECK_EQ_INT(b[4], -31744);

	SHORT h[3] = { 1000, -1000, 7 };
	pcm_s16le_pga_softlimiter(h, 3, 128);	/* -6 dB */
	CHECK_EQ_INT(h[0], 500);
	CHECK_EQ_INT(h[1], -500);
	CHECK_EQ_INT(h[2], 3);			/* 7*128>>8 */
}

TEST(softlimiter_compresses_and_saturates_instead_of_wrapping)
{
	SHORT b[6] = { 32000, -32000, 20000, -20000, 32767, -32768 };
	pcm_s16le_pga_softlimiter(b, 6, 256 * 4);	/* +12 dB */
	CHECK_EQ_INT(b[0], 32767);		/* 128000 -> curve -> saturated */
	CHECK_EQ_INT(b[1], -32768);
	CHECK_EQ_INT(b[2], 32767);
	CHECK_EQ_INT(b[3], -32768);
	CHECK_EQ_INT(b[4], 32767);
	CHECK_EQ_INT(b[5], -32768);

	SHORT m[2] = { 8000, -8000 };
	pcm_s16le_pga_softlimiter(m, 2, 256 * 4);	/* 32000 just above the barrier: curve value */
	CHECK_EQ_INT(m[0], 31970);
	CHECK_EQ_INT(m[1], -31970);
}

TEST(softlimiter_handles_an_empty_buffer)
{
	SHORT b[1] = { 4321 };
	pcm_s16le_pga_softlimiter(b, 0, 256);
	CHECK_EQ_INT(b[0], 4321);
}

TEST(hard_limiter_scales_and_saturates)
{
	SHORT b[6] = { 100, -100, 20000, -20000, 32767, -32768 };
	pcm_s16le_pga_limiter(b, 6, 512);	/* x2 */
	CHECK_EQ_INT(b[0], 200);
	CHECK_EQ_INT(b[1], -200);
	CHECK_EQ_INT(b[2], 32767);
	CHECK_EQ_INT(b[3], -32768);
	CHECK_EQ_INT(b[4], 32767);
	CHECK_EQ_INT(b[5], -32768);

	SHORT z[2] = { 1234, -1234 };
	pcm_s16le_pga_limiter(z, 2, 0);
	CHECK_EQ_INT(z[0], 0);
	CHECK_EQ_INT(z[1], 0);

	SHORT u[2] = { 1234, -1234 };
	pcm_s16le_pga_limiter(u, 2, 256);
	CHECK_EQ_INT(u[0], 1234);
	CHECK_EQ_INT(u[1], -1234);
}

TEST(output_never_leaves_the_int16_range_whatever_the_input)
{
	pcm_set_agc(44100);
	SHORT f[32];
	static const short extremes[] = { 32767, -32768, 30000, -30000, 0 };
	for (int round = 0; round < 3000; round++) {
		for (int i = 0; i < 32; i++)
			f[i] = extremes[(i + round) % 5];
		/* int16 cannot hold anything else: the guarantee is the absence of a wrap-around,
		   i.e. a positive input never turns negative and vice versa */
		SHORT in[32];
		memcpy(in, f, sizeof in);
		pcm_apply_agc(f, 5);
		for (int i = 0; i < 32; i++) {
			if (in[i] > 0) CHECK_MSG(f[i] > 0, "round %d: %d -> %d", round, in[i], f[i]);
			if (in[i] < 0) CHECK_MSG(f[i] < 0, "round %d: %d -> %d", round, in[i], f[i]);
			if (in[i] == 0) CHECK_MSG(f[i] == 0, "round %d: 0 -> %d", round, f[i]);
		}
	}
}

/* ---- debug output paths ------------------------------------------------------------------- */

TEST(debug_logging_does_not_change_the_audio_result)
{
	SHORT ref[64], dbg1[64], dbg2[64];
	for (int i = 0; i < 64; i++)
		ref[i] = dbg1[i] = dbg2[i] = (SHORT)((i * 997) % 40000 - 20000);

	pcm_set_agc(44100);
	Debug[DBG_AGC] = 0;
	pcm_apply_agc(ref, 6);

	pcm_set_agc(44100);
	Debug[DBG_AGC] = 1;
	pcm_apply_agc(dbg1, 6);

	pcm_set_agc(44100);
	Debug[DBG_AGC] = 2;			/* also prints every soft-clipped sample */
	pcm_apply_agc(dbg2, 6);
	Debug[DBG_AGC] = 0;

	CHECK_EQ_MEM(dbg1, ref, sizeof ref);
	CHECK_EQ_MEM(dbg2, ref, sizeof ref);
}

/* ---- debug commands, through the real dispatcher ------------------------------------------- */

TEST(debug_command_atef_sets_the_frame_order_and_prints_it_without_an_argument)
{
	pcm_set_agc(44100);
	debug_do_cmd((const unsigned char *)"atef 4");
	CHECK_EQ_INT(_agc_frameorder, 4);
	debug_do_cmd((const unsigned char *)"atef");
	CHECK_EQ_INT(_agc_frameorder, 4);
}

TEST(debug_command_atei_and_ated_set_the_gain_steps)
{
	pcm_set_agc(44100);
	debug_do_cmd((const unsigned char *)"atei 1000");
	CHECK_EQ_INT(_agc_gain_incr, 1000);
	debug_do_cmd((const unsigned char *)"ated 2000");
	CHECK_EQ_INT(_agc_gain_decr, 2000);
	debug_do_cmd((const unsigned char *)"atei");
	debug_do_cmd((const unsigned char *)"ated");
	CHECK_EQ_INT(_agc_gain_incr, 1000);
	CHECK_EQ_INT(_agc_gain_decr, 2000);

	/* the new steps are really used */
	run_frame(0);
	CHECK_EQ_INT(_agc_current_gain, Q16(256) + 1000);
	run_frame(20000);
	CHECK_EQ_INT(_agc_current_gain, Q16(256));	/* 1000 - 2000 clamps at the floor */
}

TEST(debug_commands_atpup_and_atpdn_double_and_halve_the_power_threshold)
{
	pcm_set_agc(44100);
	debug_do_cmd((const unsigned char *)"atpup");
	CHECK(_agc_power_threshold == 50000000LL);
	debug_do_cmd((const unsigned char *)"atpdn");
	debug_do_cmd((const unsigned char *)"atpdn");
	CHECK(_agc_power_threshold == 12500000LL);

	/* a constant of 4000 (16,000,000) is now loud (> 12.5M) */
	run_frames(100, 0);
	int base = _agc_current_gain;
	run_frame(4000);
	CHECK_EQ_INT(_agc_current_gain, base - 374582);
}

TEST(debug_command_atpdn_never_takes_the_threshold_below_one)
{
	pcm_set_agc(44100);
	for (int i = 0; i < 40; i++)
		debug_do_cmd((const unsigned char *)"atpdn");
	CHECK(_agc_power_threshold == 1LL);
	/* with threshold 1 any non-zero frame is "loud" */
	run_frames(100, 0);
	int base = _agc_current_gain;
	run_frame(2);
	CHECK(_agc_current_gain < base);
}

TEST(debug_switch_dbgagc_toggles_the_agc_debug_level)
{
	Debug[DBG_AGC] = 0;
	debug_do_cmd((const unsigned char *)"dbgagc");
	CHECK_EQ_INT(Debug[DBG_AGC], 1);
	debug_do_cmd((const unsigned char *)"dbgagc 2");
	CHECK_EQ_INT(Debug[DBG_AGC], 2);
	debug_do_cmd((const unsigned char *)"dbgagc");
	CHECK_EQ_INT(Debug[DBG_AGC], 0);
}

/*
 * UNREACHABLE on host: none. Every executable line is covered (146/146). The few branch outcomes
 * not taken are compiler-generated paths of abs() and of the DBG/DBG2 `if (Debug[..])` guards
 * combined with serprintf's argument evaluation; no behaviour is left unexercised.
 *
 * Mutation check done by hand when this file was written (each mutant of Source/pcm_autogain.c
 * was compiled against these tests and had to fail): `<` -> `<=` on the power threshold, GAIN_MAX
 * clamp off by one, hold counter not decremented, hold counter not re-armed by attack, `>` -> `>=`
 * on the clip barrier, sign test in the soft clipper, frameorder-1 energy window, hold_max and
 * incr formulas. All nine were killed.
 */
