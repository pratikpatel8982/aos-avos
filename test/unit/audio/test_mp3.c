/*
 * test_mp3.c -- unit tests for Source/mp3.c (MPEG audio header parsing and frame synchronisation).
 *
 * Public API of the real, instrumented mp3.o. Inputs are real MPEG audio bytes: frames assembled
 * header-by-header following ISO 11172-3 / 13818-3, and the real encoder output in
 * fixtures/media/sine*.mp3, whose stream facts are cross-checked against libavformat.
 *
 * Notes on the source (see the known_defect_ tests and the end of the file):
 *  - the 4-bit "layer 1" bitrate row of mp3_rates is the Layer II table and the frame length uses
 *    the Layer II/III formula (144 * br / fs) for Layer I as well.
 *  - MP3_check_header() reports lsf == 0 for MPEG-2 Layer II/I; that is right for the frame
 *    length formula (Layer II frames always hold 1152 samples) and is pinned as is.
 */
#include "avos_test.h"
#include "av.h"
#include "debug.h"
#include "mp3.h"

#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* second header byte for each (version, layer) with the CRC bit set ("no CRC") */
#define H1_MPEG1_L3	0xFB
#define H1_MPEG2_L3	0xF3
#define H1_MPEG25_L3	0xE3
#define H1_MPEG1_L2	0xFD
#define H1_MPEG2_L2	0xF5
#define H1_MPEG1_L1	0xFF
#define H1_MPEG2_L1	0xF7

/* Write a 4 byte header at p. mode 3 = mono. */
static void put_header(UCHAR *p, int h1, int bitrate_idx, int sr_idx, int padding, int mode)
{
	p[0] = 0xFF;
	p[1] = (UCHAR)h1;
	p[2] = (UCHAR)((bitrate_idx << 4) | (sr_idx << 2) | (padding << 1));
	p[3] = (UCHAR)(mode << 6);
}

/* A buffer of two back-to-back frames of `len` bytes starting at `lead` (zero bytes before and after). */
static UCHAR *two_frames(int lead, int len, int h1, int br, int sr, int pad, int mode, int *total)
{
	int size = lead + 2 * len + 8;
	UCHAR *b = calloc(1, size);
	REQUIRE_NOT_NULL(b);
	put_header(b + lead, h1, br, sr, pad, mode);
	put_header(b + lead + len, h1, br, sr, pad, mode);
	*total = size;
	return b;
}

static int frame_length(int h1, int br, int sr, int mode, int *MPEGp, int *lsfp, int *layerp)
{
	MP3_FRAME fr;
	UCHAR h[4];
	memset(&fr, 0, sizeof fr);
	put_header(h, h1, br, sr, 0, mode);
	MP3_check_header(h[0], h[1], &fr.MPEG, &fr.lsf, &fr.layer);
	MP3_get_frame_stats(&fr, h);
	if (MPEGp) *MPEGp = fr.MPEG;
	if (lsfp) *lsfp = fr.lsf;
	if (layerp) *layerp = fr.layer;
	return fr.length;
}

/* ---- tables ------------------------------------------------------------------------------- */

TEST(sampling_frequency_table_matches_the_standard)
{
	CHECK_EQ_INT(mp3_freqs[MPEG_1][0], 44100);
	CHECK_EQ_INT(mp3_freqs[MPEG_1][1], 48000);
	CHECK_EQ_INT(mp3_freqs[MPEG_1][2], 32000);
	CHECK_EQ_INT(mp3_freqs[MPEG_2][0], 22050);
	CHECK_EQ_INT(mp3_freqs[MPEG_2][1], 24000);
	CHECK_EQ_INT(mp3_freqs[MPEG_2][2], 16000);
	CHECK_EQ_INT(mp3_freqs[MPEG_2_5][0], 11025);
	CHECK_EQ_INT(mp3_freqs[MPEG_2_5][1], 12000);
	CHECK_EQ_INT(mp3_freqs[MPEG_2_5][2], 8000);
}

TEST(layer_iii_bitrate_tables_match_the_standard)
{
	static const int mpeg1[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
	static const int mpeg2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
	for (int i = 0; i < 15; i++) {
		CHECK_MSG(mp3_rates[LAYER_3][MPEG_1][i] == mpeg1[i], "L3 MPEG1 idx %d = %ld", i, mp3_rates[LAYER_3][MPEG_1][i]);
		CHECK_MSG(mp3_rates[LAYER_3][MPEG_2][i] == mpeg2[i], "L3 MPEG2 idx %d = %ld", i, mp3_rates[LAYER_3][MPEG_2][i]);
		CHECK_MSG(mp3_rates[LAYER_3][MPEG_2_5][i] == mpeg2[i], "L3 MPEG2.5 idx %d = %ld", i, mp3_rates[LAYER_3][MPEG_2_5][i]);
	}
}

TEST(layer_ii_mpeg1_bitrate_table_matches_the_standard)
{
	static const int l2[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
	for (int i = 0; i < 15; i++)
		CHECK_MSG(mp3_rates[LAYER_2][MPEG_1][i] == l2[i], "L2 MPEG1 idx %d = %ld", i, mp3_rates[LAYER_2][MPEG_1][i]);
}

/* ---- MP3_check_header ---------------------------------------------------------------------- */

struct hdr_case { int h0, h1, MPEG, lsf, layer; };

TEST(check_header_recognises_every_supported_version_and_layer)
{
	static const struct hdr_case c[] = {
		{ 0xFF, 0xFA, MPEG_1,   0, LAYER_3 }, { 0xFF, 0xF2, MPEG_2,   1, LAYER_3 },
		{ 0xFF, 0xE2, MPEG_2_5, 1, LAYER_3 }, { 0xFF, 0xFC, MPEG_1,   0, LAYER_2 },
		{ 0xFF, 0xF4, MPEG_2,   0, LAYER_2 }, { 0xFF, 0xFE, MPEG_1,   0, LAYER_1 },
		{ 0xFF, 0xF6, MPEG_2,   0, LAYER_1 },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
		int MPEG = 99, lsf = 99, layer = 99;
		int r = MP3_check_header(c[i].h0, c[i].h1, &MPEG, &lsf, &layer);
		CHECK_MSG(r == 0, "%02X%02X rejected", c[i].h0, c[i].h1);
		CHECK_MSG(MPEG == c[i].MPEG && lsf == c[i].lsf && layer == c[i].layer,
			  "%02X%02X -> MPEG %d lsf %d layer %d", c[i].h0, c[i].h1, MPEG, lsf, layer);
	}
}

TEST(check_header_ignores_the_crc_protection_bit)
{
	static const int with_crc[] = { 0xFA, 0xF2, 0xE2, 0xFC, 0xF4, 0xFE, 0xF6 };
	for (size_t i = 0; i < sizeof with_crc / sizeof with_crc[0]; i++) {
		int a_MPEG, a_lsf, a_layer, b_MPEG, b_lsf, b_layer;
		CHECK_EQ_INT(MP3_check_header(0xFF, with_crc[i], &a_MPEG, &a_lsf, &a_layer), 0);
		CHECK_EQ_INT(MP3_check_header(0xFF, with_crc[i] | 1, &b_MPEG, &b_lsf, &b_layer), 0);
		CHECK(a_MPEG == b_MPEG && a_lsf == b_lsf && a_layer == b_layer);
	}
}

TEST(check_header_rejects_everything_that_is_not_a_supported_header)
{
	static const struct { int h0, h1; } bad[] = {
		{ 0x00, 0x00 }, { 0xFE, 0xFB }, { 0xFF, 0x00 }, { 0xFF, 0xFB & 0xE0 },
		{ 0xFF, 0xF8 },		/* MPEG-1, reserved layer 00 */
		{ 0xFF, 0xF0 },		/* MPEG-2, reserved layer */
		{ 0xFF, 0xEA },		/* reserved version 01 */
		{ 0xFF, 0xE4 },		/* MPEG-2.5 layer II: not supported here */
		{ 0xFF, 0xE6 },		/* MPEG-2.5 layer I: not supported here */
		{ 0xFF, 0xF1 & 0xF0 }, { 0x49, 0x44 },	/* "ID" of an ID3 tag */
		{ 0x7F, 0xFB },
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		int MPEG = 7, lsf = 7, layer = 7;
		CHECK_MSG(MP3_check_header(bad[i].h0, bad[i].h1, &MPEG, &lsf, &layer) == 1, "%02X%02X accepted", bad[i].h0, bad[i].h1);
		CHECK_MSG(MPEG == -1 && lsf == -1 && layer == -1, "%02X%02X: outputs %d %d %d", bad[i].h0, bad[i].h1, MPEG, lsf, layer);
	}
}

TEST(check_header_accepts_null_output_pointers)
{
	CHECK_EQ_INT(MP3_check_header(0xFF, 0xFB, NULL, NULL, NULL), 0);
	CHECK_EQ_INT(MP3_check_header(0x12, 0x34, NULL, NULL, NULL), 1);
	int layer = -5;
	CHECK_EQ_INT(MP3_check_header(0xFF, 0xFB, NULL, NULL, &layer), 0);
	CHECK_EQ_INT(layer, LAYER_3);
}

/* ---- MP3_get_frame_stats ------------------------------------------------------------------- */

TEST(frame_stats_decode_every_header_field)
{
	MP3_FRAME fr;
	memset(&fr, 0, sizeof fr);
	fr.MPEG = MPEG_1; fr.lsf = 0; fr.layer = LAYER_3;
	/* FF FB | bitrate 9 (128k), sr 1 (48k), padding 1, private 1 | stereo mode 2, ext 3, copyright, original, emphasis 2 */
	UCHAR h[4] = { 0xFF, 0xFB, (UCHAR)((9 << 4) | (1 << 2) | (1 << 1) | 1), (UCHAR)((2 << 6) | (3 << 4) | (1 << 3) | (1 << 2) | 2) };
	MP3_get_frame_stats(&fr, h);
	CHECK_EQ_INT(fr.protection, 1);
	CHECK_EQ_INT(fr.bitrate_index, 9);
	CHECK_EQ_INT(fr.sr_index, 1);
	CHECK_EQ_INT(fr.padding, 1);
	CHECK_EQ_INT(fr.private, 1);
	CHECK_EQ_INT(fr.mode, 2);
	CHECK_EQ_INT(fr.ch, 2);
	CHECK_EQ_INT(fr.mode_ext, 3);
	CHECK_EQ_INT(fr.copyright, 1);
	CHECK_EQ_INT(fr.original, 1);
	CHECK_EQ_INT(fr.emphasis, 2);
	CHECK_EQ_INT(fr.length, 144000 * 128 / 48000);	/* 384 */

	/* all the flag bits clear */
	UCHAR z[4] = { 0xFF, 0xFA, (UCHAR)(1 << 4), 0 };
	MP3_get_frame_stats(&fr, z);
	CHECK_EQ_INT(fr.protection, 0);
	CHECK_EQ_INT(fr.padding, 0);
	CHECK_EQ_INT(fr.private, 0);
	CHECK_EQ_INT(fr.copyright, 0);
	CHECK_EQ_INT(fr.original, 0);
	CHECK_EQ_INT(fr.emphasis, 0);
	CHECK_EQ_INT(fr.mode, 0);
}

TEST(frame_stats_side_info_offset_depends_on_version_and_channel_mode)
{
	MP3_FRAME fr;
	memset(&fr, 0, sizeof fr);
	fr.layer = LAYER_3;
	UCHAR st[4] = { 0xFF, 0xFB, 0x90, 0x00 };	/* stereo */
	UCHAR mo[4] = { 0xFF, 0xFB, 0x90, 0xC0 };	/* mono */

	fr.MPEG = MPEG_1; fr.lsf = 0;
	MP3_get_frame_stats(&fr, st); CHECK_EQ_INT(fr.data_start, 36); CHECK_EQ_INT(fr.ch, 2);
	MP3_get_frame_stats(&fr, mo); CHECK_EQ_INT(fr.data_start, 21); CHECK_EQ_INT(fr.ch, 1);

	fr.MPEG = MPEG_2; fr.lsf = 1;
	MP3_get_frame_stats(&fr, st); CHECK_EQ_INT(fr.data_start, 21);
	MP3_get_frame_stats(&fr, mo); CHECK_EQ_INT(fr.data_start, 13);

	fr.MPEG = MPEG_2_5;
	MP3_get_frame_stats(&fr, st); CHECK_EQ_INT(fr.data_start, 21);
	MP3_get_frame_stats(&fr, mo); CHECK_EQ_INT(fr.data_start, 13);
}

TEST(frame_stats_length_follows_144_times_bitrate_over_sample_rate)
{
	int MPEG, lsf, layer;
	/* MPEG-1 layer III: 128 kbit/s @ 44.1 kHz -> 417 bytes (without padding) */
	CHECK_EQ_INT(frame_length(H1_MPEG1_L3, 9, 0, 0, &MPEG, &lsf, &layer), 417);
	CHECK(MPEG == MPEG_1 && lsf == 0 && layer == LAYER_3);
	/* 320 kbit/s @ 32 kHz -> 1440, 32 kbit/s @ 48 kHz -> 96 */
	CHECK_EQ_INT(frame_length(H1_MPEG1_L3, 14, 2, 0, NULL, NULL, NULL), 1440);
	CHECK_EQ_INT(frame_length(H1_MPEG1_L3, 1, 1, 0, NULL, NULL, NULL), 96);
	/* MPEG-2 layer III halves the rate: 64 kbit/s @ 22.05 kHz -> 144000*64/(22050*2) = 208 */
	CHECK_EQ_INT(frame_length(H1_MPEG2_L3, 8, 0, 0, NULL, NULL, NULL), 208);
	/* MPEG-2.5: 32 kbit/s @ 8 kHz -> 144000*32/(8000*2) = 288 */
	CHECK_EQ_INT(frame_length(H1_MPEG25_L3, 4, 2, 0, NULL, NULL, NULL), 288);
	/* layer II MPEG-1: 128 kbit/s @ 44.1 kHz -> 417 ; 384 kbit/s @ 48 kHz -> 1152 */
	CHECK_EQ_INT(frame_length(H1_MPEG1_L2, 8, 0, 0, NULL, NULL, NULL), 417);
	CHECK_EQ_INT(frame_length(H1_MPEG1_L2, 14, 1, 0, NULL, NULL, NULL), 1152);
	/* layer II MPEG-2 uses lsf 0 (1152 samples per frame): 64 kbit/s @ 22.05 kHz -> 417 */
	CHECK_EQ_INT(frame_length(H1_MPEG2_L2, 8, 0, 0, NULL, NULL, NULL), 144000 * 64 / 22050);
}

TEST(frame_stats_give_length_zero_for_every_invalid_combination)
{
	/* free-format bitrate and the forbidden index 15 */
	CHECK_EQ_INT(frame_length(H1_MPEG1_L3, 0, 0, 0, NULL, NULL, NULL), 0);
	CHECK_EQ_INT(frame_length(H1_MPEG1_L3, 15, 0, 0, NULL, NULL, NULL), 0);
	/* reserved sampling frequency index 3 */
	CHECK_EQ_INT(frame_length(H1_MPEG1_L3, 9, 3, 0, NULL, NULL, NULL), 0);

	/* the MP3_FRAME was never identified (layer -1) */
	MP3_FRAME fr;
	memset(&fr, 0, sizeof fr);
	fr.MPEG = MPEG_1; fr.lsf = 0; fr.layer = -1;
	UCHAR h[4] = { 0xFF, 0xFB, 0x90, 0x00 };
	MP3_get_frame_stats(&fr, h);
	CHECK_EQ_INT(fr.length, 0);

	fr.layer = LAYER_3; fr.MPEG = MPEG_2_5 + 1;	/* unknown version */
	MP3_get_frame_stats(&fr, h);
	CHECK_EQ_INT(fr.length, 0);

	fr.MPEG = MPEG_1; fr.lsf = 2;			/* lsf out of range */
	MP3_get_frame_stats(&fr, h);
	CHECK_EQ_INT(fr.length, 0);
}

TEST(known_defect_layer_i_frame_length_uses_the_layer_ii_iii_formula)
{
	/*
	 * ISO 11172-3: a Layer I frame is (12 * bitrate / samplerate + padding) * 4 bytes, i.e.
	 * floor(12*64000/44100)*4 = 68 bytes for 64 kbit/s @ 44.1 kHz, and 64 kbit/s is bitrate index 2
	 * of the Layer I table {32,64,96,...,448}.
	 * mp3.c indexes the Layer II table there (index 2 = 48 kbit/s) and applies 144000*br/fs, so it
	 * reports 144000*48/44100 = 156. Correct value: 68. Flip this test when the code is fixed.
	 */
	int layer;
	int len = frame_length(H1_MPEG1_L1, 2, 0, 0, NULL, NULL, &layer);
	CHECK_EQ_INT(layer, LAYER_1);
	CHECK_EQ_INT(mp3_rates[LAYER_1][MPEG_1][2], 48);	/* standard says 64 */
	CHECK_EQ_INT(len, 156);					/* standard says 68 */
}

/* ---- MP3_find_sync ------------------------------------------------------------------------- */

TEST(find_sync_locates_the_first_valid_frame_and_describes_the_stream)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(0, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);	/* 128k, 44.1k, stereo */
	MP3_FRAME fr;
	AUDIO_PROPERTIES audio;
	memset(&audio, 0xAA, sizeof audio);

	CHECK_EQ_INT(MP3_find_sync(&audio, b, total, &fr, &ofs), 0);
	CHECK_EQ_INT(ofs, 0);
	CHECK_EQ_INT(audio.bytesPerSec, 128 * 125);
	CHECK_EQ_INT(audio.samplesPerSec, 44100);
	CHECK_EQ_INT(audio.channels, 2);
	CHECK_EQ_INT(audio.bitsPerSample, 16);
	CHECK_EQ_INT(audio.format, WAVE_FORMAT_MPEGLAYER3);
	CHECK_EQ_INT(audio.blockAlign, 1);
	CHECK_EQ_INT(audio.scale, 1);
	CHECK_EQ_INT(audio.rate, 128 * 125);
	CHECK_EQ_INT(audio.vbr, 0);
	CHECK_EQ_INT(audio.frames, 0);
	CHECK_EQ_INT(audio.valid, 1);
	CHECK_EQ_INT(fr.length, 417);
	CHECK_EQ_INT(fr.MPEG, MPEG_1);
	CHECK_EQ_INT(fr.layer, LAYER_3);
	free(b);
}

TEST(find_sync_skips_leading_garbage_and_reports_the_offset)
{
	static const int leads[] = { 1, 7, 100, 1000 };
	for (size_t i = 0; i < sizeof leads / sizeof leads[0]; i++) {
		int total, ofs = -1;
		UCHAR *b = two_frames(leads[i], 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
		MP3_FRAME fr;
		CHECK_MSG(MP3_find_sync(NULL, b, total, &fr, &ofs) == 0, "lead %d: no sync", leads[i]);
		CHECK_MSG(ofs == leads[i], "lead %d: offset %d", leads[i], ofs);
		free(b);
	}
}

TEST(find_sync_ignores_a_lone_header_whose_follow_up_is_not_a_frame)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(0, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	memset(b + 417, 0, 4);					/* destroy the second header */
	MP3_FRAME fr;
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 1);
	CHECK_EQ_INT(ofs, total);
	free(b);
}

TEST(find_sync_accepts_a_padded_frame_whose_successor_is_one_byte_late)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(3, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	memmove(b + 3 + 418, b + 3 + 417, 4);			/* second header at length + 1 */
	b[3 + 417] = 0;
	MP3_FRAME fr;
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 0);
	CHECK_EQ_INT(ofs, 3);
	free(b);
}

TEST(find_sync_rejects_a_successor_that_is_two_bytes_late)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(0, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	memmove(b + 419, b + 417, 4);
	b[417] = b[418] = 0;
	MP3_FRAME fr;
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 1);
	free(b);
}

TEST(find_sync_rejects_a_successor_of_a_different_version_or_layer)
{
	static const int other_h1[] = { H1_MPEG2_L3, H1_MPEG1_L2, H1_MPEG25_L3 };
	for (size_t i = 0; i < sizeof other_h1 / sizeof other_h1[0]; i++) {
		int total, ofs = -1;
		UCHAR *b = two_frames(0, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
		put_header(b + 417, other_h1[i], 9, 0, 0, 0);
		MP3_FRAME fr;
		CHECK_MSG(MP3_find_sync(NULL, b, total, &fr, &ofs) == 1, "successor 0x%02X accepted", other_h1[i]);
		free(b);
	}
}

TEST(find_sync_requires_the_whole_frame_to_fit_in_the_buffer)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(0, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	MP3_FRAME fr;
	/* frame ends exactly at size-1: sc + length > size - 1 is false -> accepted */
	CHECK_EQ_INT(MP3_find_sync(NULL, b, 417 + 4 + 1, &fr, &ofs), 0);
	/* one byte less and the header of the next frame is cut: rejected */
	CHECK_EQ_INT(MP3_find_sync(NULL, b, 417, &fr, &ofs), 1);
	CHECK_EQ_INT(ofs, 417);
	free(b);
}

TEST(find_sync_skips_headers_with_a_reserved_sampling_rate_or_free_format_bitrate)
{
	int total, ofs = -1;
	MP3_FRAME fr;
	UCHAR *b = two_frames(0, 100, H1_MPEG1_L3, 9, 3, 0, 0, &total);	/* sr index 3 */
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 1);
	free(b);

	b = two_frames(0, 100, H1_MPEG1_L3, 0, 0, 0, 0, &total);		/* bitrate index 0 */
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 1);
	free(b);

	b = two_frames(0, 100, H1_MPEG1_L3, 15, 0, 0, 0, &total);		/* bitrate index 15 */
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 1);
	free(b);
}

TEST(find_sync_recovers_after_a_false_start_inside_garbage)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(50, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	put_header(b + 10, H1_MPEG1_L3, 9, 0, 0, 0);		/* an orphan header with no successor */
	MP3_FRAME fr;
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, &ofs), 0);
	CHECK_EQ_INT(ofs, 50);
	free(b);
}

TEST(find_sync_reports_mono_and_each_sampling_rate_family)
{
	struct { int h1, br, sr, mode, rate_kbps, sps, ch; } c[] = {
		{ H1_MPEG1_L3, 9, 1, 3, 128, 48000, 1 },	/* MPEG-1 mono */
		{ H1_MPEG1_L3, 5, 2, 1, 64, 32000, 2 },		/* joint stereo */
		{ H1_MPEG2_L3, 8, 0, 0, 64, 22050, 2 },
		{ H1_MPEG2_L3, 8, 2, 2, 64, 16000, 2 },
		{ H1_MPEG25_L3, 4, 0, 3, 32, 11025, 1 },
		{ H1_MPEG25_L3, 4, 2, 0, 32, 8000, 2 },
		{ H1_MPEG1_L2, 8, 0, 0, 128, 44100, 2 },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
		int len = frame_length(c[i].h1, c[i].br, c[i].sr, c[i].mode, NULL, NULL, NULL);
		REQUIRE(len > 0);
		int total, ofs = -1;
		UCHAR *b = two_frames(2, len, c[i].h1, c[i].br, c[i].sr, 0, c[i].mode, &total);
		MP3_FRAME fr;
		AUDIO_PROPERTIES audio;
		memset(&audio, 0, sizeof audio);
		CHECK_MSG(MP3_find_sync(&audio, b, total, &fr, &ofs) == 0, "case %zu: no sync", i);
		CHECK_MSG(audio.bytesPerSec == c[i].rate_kbps * 125, "case %zu: bytesPerSec %d", i, audio.bytesPerSec);
		CHECK_MSG(audio.samplesPerSec == c[i].sps, "case %zu: rate %d", i, audio.samplesPerSec);
		CHECK_MSG(audio.channels == c[i].ch, "case %zu: channels %d", i, audio.channels);
		CHECK_MSG(ofs == 2, "case %zu: offset %d", i, ofs);
		free(b);
	}
}

TEST(find_sync_handles_empty_and_header_free_buffers)
{
	UCHAR zeros[64] = { 0 };
	int ofs = -1;
	MP3_FRAME fr;
	CHECK_EQ_INT(MP3_find_sync(NULL, zeros, 0, &fr, &ofs), 1);
	CHECK_EQ_INT(ofs, 0);
	ofs = -1;
	CHECK_EQ_INT(MP3_find_sync(NULL, zeros, sizeof zeros, &fr, &ofs), 1);
	CHECK_EQ_INT(ofs, sizeof zeros);
}

TEST(find_sync_does_not_require_the_optional_output_pointers)
{
	int total;
	UCHAR *b = two_frames(0, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	MP3_FRAME fr;
	CHECK_EQ_INT(MP3_find_sync(NULL, b, total, &fr, NULL), 0);
	UCHAR zeros[8] = { 0 };
	CHECK_EQ_INT(MP3_find_sync(NULL, zeros, sizeof zeros, &fr, NULL), 1);
	free(b);
}

TEST(find_sync2_is_find_sync_without_audio_properties)
{
	int total, ofs = -1;
	UCHAR *b = two_frames(13, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	CHECK_EQ_INT(MP3_find_sync2(b, total, &ofs), 0);
	CHECK_EQ_INT(ofs, 13);
	b[13] = 0;
	CHECK_EQ_INT(MP3_find_sync2(b, total, &ofs), 1);
	free(b);
}

TEST(parser_debug_logging_does_not_change_the_result)
{
	int total, ofs0 = -1, ofs1 = -1, ofs2 = -1;
	UCHAR *b = two_frames(5, 417, H1_MPEG1_L3, 9, 0, 0, 0, &total);
	MP3_FRAME fr;
	AUDIO_PROPERTIES a0, a1, a2;
	memset(&a0, 0, sizeof a0); memset(&a1, 0, sizeof a1); memset(&a2, 0, sizeof a2);

	Debug[DBG_PARSER] = 0;
	CHECK_EQ_INT(MP3_find_sync(&a0, b, total, &fr, &ofs0), 0);
	Debug[DBG_PARSER] = 1;
	CHECK_EQ_INT(MP3_find_sync(&a1, b, total, &fr, &ofs1), 0);
	Debug[DBG_PARSER] = 2;
	CHECK_EQ_INT(MP3_find_sync(&a2, b, total, &fr, &ofs2), 0);	/* also logs the header and the size check */
	CHECK_EQ_INT(MP3_find_sync(NULL, b, 417, &fr, &ofs2), 1);	/* "sc + length > size" log */
	Debug[DBG_PARSER] = 0;

	CHECK(ofs0 == 5 && ofs1 == 5 && ofs2 == 417);
	CHECK_EQ_INT(a1.samplesPerSec, a0.samplesPerSec);
	CHECK_EQ_INT(a2.bytesPerSec, a0.bytesPerSec);
	free(b);
}

/* ---- real encoder output ------------------------------------------------------------------ */

static UCHAR *slurp(const char *rel, int *size)
{
	FILE *f = fopen(fixture_path(rel), "rb");
	REQUIRE_NOT_NULL(f);
	fseek(f, 0, SEEK_END);
	*size = (int)ftell(f);
	fseek(f, 0, SEEK_SET);
	UCHAR *b = malloc(*size);
	REQUIRE_NOT_NULL(b);
	REQUIRE(fread(b, 1, *size, f) == (size_t)*size);
	fclose(f);
	return b;
}

static void check_against_libavformat(const char *rel)
{
	int size, ofs = -1;
	UCHAR *b = slurp(rel, &size);
	MP3_FRAME fr;
	AUDIO_PROPERTIES audio;
	memset(&audio, 0, sizeof audio);

	CHECK_EQ_INT(MP3_find_sync(&audio, b, size, &fr, &ofs), 0);

	AVFormatContext *ic = NULL;
	REQUIRE(avformat_open_input(&ic, fixture_path(rel), NULL, NULL) == 0);
	REQUIRE(avformat_find_stream_info(ic, NULL) >= 0);
	int idx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	REQUIRE(idx >= 0);
	AVCodecParameters *par = ic->streams[idx]->codecpar;
	CHECK_EQ_INT(par->codec_id, AV_CODEC_ID_MP3);
	CHECK_EQ_INT(audio.samplesPerSec, par->sample_rate);
	CHECK_EQ_INT(audio.channels, par->ch_layout.nb_channels);
	avformat_close_input(&ic);

	/* the frame found must be a real sync word, and the one at offset+length(+1) too */
	CHECK(b[ofs] == 0xFF && (b[ofs + 1] & 0xE0) == 0xE0);
	CHECK(ofs + fr.length + 1 < size);
	int next_ok = (b[ofs + fr.length] == 0xFF && (b[ofs + fr.length + 1] & 0xE0) == 0xE0) ||
		      (b[ofs + fr.length + 1] == 0xFF && (b[ofs + fr.length + 2] & 0xE0) == 0xE0);
	CHECK(next_ok);
	free(b);
}

TEST(real_encoder_output_sine_mp3_agrees_with_libavformat)
{
	check_against_libavformat("media/sine.mp3");
}

TEST(real_encoder_output_with_an_id3v2_tag_in_front_is_synchronised_past_the_tag)
{
	check_against_libavformat("media/sine_id3.mp3");

	int size, ofs = -1, ofs_plain = -1;
	UCHAR *tagged = slurp("media/sine_id3.mp3", &size);
	CHECK_EQ_INT(MP3_find_sync2(tagged, size, &ofs), 0);
	free(tagged);
	int psize;
	UCHAR *plain = slurp("media/sine.mp3", &psize);
	CHECK_EQ_INT(MP3_find_sync2(plain, psize, &ofs_plain), 0);
	free(plain);
	CHECK(ofs > ofs_plain);				/* the longer tag pushes the first frame further */
	CHECK(ofs >= 10 + 0x43);			/* ID3v2 header (10) + the tag body size of this fixture */
}

TEST(real_non_mp3_data_has_no_sync)
{
	int size, ofs = -1;
	UCHAR *wav = slurp("media/sine.wav", &size);
	CHECK_EQ_INT(MP3_find_sync2(wav, size, &ofs), 1);
	CHECK_EQ_INT(ofs, size);
	free(wav);
}

/*
 * Source observations (reported, not fixed here):
 *  - MP3_find_sync reads p[1] at the last position of the buffer and verify_header may read one byte
 *    past `size` (p[sc + length + 1] with sc + length == size - 1): a one byte over-read on exactly
 *    sized buffers. Not reproducible without a sanitizer, so it is not pinned by a test.
 *  - Frame length ignores the padding bit; MP3_find_sync compensates by also trying length + 1.
 */
