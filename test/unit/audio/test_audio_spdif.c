/*
 * test_audio_spdif.c -- unit tests for Source/audio_spdif.c (compressed audio passthrough:
 * IEC 61937 wrapping through FFmpeg's spdif muxer, raw codec frames for "mode 2", HDMI capability
 * flags and the "spdif" audio decoder registration).
 *
 * White box: the real source file is compiled into the test (the sim target does not link it and
 * CONFIG_SPDIF is not defined there), so statics such as passthrough_on, hdmi_audio_codecs_flag,
 * fctxt/avctx/aparser are reachable. Everything else is real: the FFmpeg spdif muxer and parsers
 * inside spdif_init(), the stream registry, libavos's recoding switch.
 *
 * Input is real compressed audio produced by the real FFmpeg encoders (spdif_media.h), and the IEC 61937
 * bursts that come out are decoded again by FFmpeg's own "spdif" demuxer, so the oracle is an
 * independent implementation of the standard: what goes in must come back out byte for byte.
 *
 * Not reachable here (see the end of the file): the CONFIG_ANDROID variants (tested in
 * test_audio_spdif_android.c) and the AC3-recoding branches, whose switch lives in libavos.c behind
 * CONFIG_SPDIF (see test_audio_spdif_recoding.c).
 */
#include "avos_test.h"
#include "Source/audio_spdif.c"
#include "spdif_media.h"

#include <libavformat/avformat.h>
#include <stdio.h>

/* ---- helpers ---------------------------------------------------------------------------------- */

static long bit(int enc) { return 1L << enc; }

static void props(AUDIO_PROPERTIES *a, int format, int rate, int channels)
{
	memset(a, 0, sizeof *a);
	a->format = format;
	a->samplesPerSec = rate;
	a->channels = channels;
	a->bitsPerSample = 16;
	a->bytesPerFrame = channels * 2;
	a->bytesPerSec = a->bytesPerFrame * rate;
}

static unsigned char *slurp(const char *rel, int *size)
{
	FILE *f = fopen(fixture_path(rel), "rb");
	REQUIRE_NOT_NULL(f);
	fseek(f, 0, SEEK_END);
	*size = (int)ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc(*size);
	REQUIRE(fread(b, 1, *size, f) == (size_t)*size);
	fclose(f);
	return b;
}

/* ---- decapsulation of IEC 61937 with FFmpeg's own spdif demuxer (the oracle) ------------------- */

typedef struct { const unsigned char *data; int len, pos; } MemRd;

static int mem_read(void *op, uint8_t *buf, int n)
{
	MemRd *m = op;
	int k = m->len - m->pos;
	if (k <= 0)
		return AVERROR_EOF;
	if (k > n)
		k = n;
	memcpy(buf, m->data + m->pos, k);
	m->pos += k;
	return k;
}

typedef struct {
	enum AVCodecID codec;
	unsigned char *payload;		/* all demuxed packets back to back */
	int size, npkts;
} Demuxed;

static int iec_demux(const unsigned char *bytes, int len, Demuxed *out)
{
	memset(out, 0, sizeof *out);
	MemRd m = { bytes, len, 0 };
	AVFormatContext *ic = avformat_alloc_context();
	unsigned char *iobuf = av_malloc(8192);
	ic->pb = avio_alloc_context(iobuf, 8192, 0, &m, mem_read, NULL, NULL);
	ic->flags |= AVFMT_FLAG_CUSTOM_IO;
	int ret = avformat_open_input(&ic, NULL, av_find_input_format("spdif"), NULL);
	if (ret < 0) {
		av_freep(&iobuf);
		return ret;
	}
	AVPacket *p = av_packet_alloc();
	while (av_read_frame(ic, p) >= 0) {
		if (out->npkts == 0)
			out->codec = ic->streams[p->stream_index]->codecpar->codec_id;
		out->payload = realloc(out->payload, out->size + p->size);
		memcpy(out->payload + out->size, p->data, p->size);
		out->size += p->size;
		out->npkts++;
		av_packet_unref(p);
	}
	av_packet_free(&p);
	AVIOContext *pb = ic->pb;
	avformat_close_input(&ic);
	av_freep(&pb->buffer);
	avio_context_free(&pb);
	return 0;
}

/* Run every packet through spdif_encapsulate (+ an EOF drain) and collect what comes out. */
typedef struct {
	unsigned char *out;
	int out_size, calls_with_output;
	long long fake_total;
	int decoded_total, fatal;
	int first_fake, last_format;
} Run;

/* Offer one packet the way the stream layer does: advance by what was consumed, and offer the rest again.
   (The parser hands back the frame it was holding and consumes nothing until it is called again.) */
static void feed(AUDIO_PROPERTIES *a, const unsigned char *data, int size, Run *r)
{
	int remaining = size, stuck = 0;
	do {
		AUDIO_FRAME fr;
		memset(&fr, 0, sizeof fr);
		int decoded = -1;
		int rv = spdif_encapsulate(a, (UCHAR *)data + (size - remaining), remaining, &fr, &decoded);
		if (rv < 0 || fr.error == STREAM_ERROR_FATAL)
			r->fatal++;
		if (fr.size > 0) {
			r->out = realloc(r->out, r->out_size + fr.size);
			memcpy(r->out + r->out_size, fr.data, fr.size);
			r->out_size += fr.size;
			r->calls_with_output++;
			r->fake_total += fr.fakeSize;
			if (!r->first_fake)
				r->first_fake = fr.fakeSize;
			r->last_format = fr.format;
		}
		if (decoded > 0)
			r->decoded_total += decoded;
		remaining -= decoded > 0 ? decoded : 0;
		stuck = (decoded <= 0 && fr.size == 0) ? stuck + 1 : 0;
	} while (remaining > 0 && stuck < 2);
}

static void run_packets(AUDIO_PROPERTIES *a, const CodedStream *cs, int drain, Run *r)
{
	memset(r, 0, sizeof *r);
	int off = 0;
	for (int i = 0; i < cs->npkts; i++) {
		feed(a, cs->data + off, cs->pkt_size[i], r);
		off += cs->pkt_size[i];
	}
	if (drain)
		feed(a, cs->data, 0, r);
}

static void setup_mode1(AUDIO_PROPERTIES *a, int format, int rate, int channels, long flags)
{
	set_hdmi_supported_audio_codecs(flags);
	spdif_set_passthrough(1);
	props(a, format, rate, channels);
	REQUIRE(spdif_init(a) == 1);
	REQUIRE(spdif_open(a) == 0);
}

/* ---- passthrough mode and HDMI capability flags ------------------------------------------------ */

TEST(passthrough_mode_is_unset_until_a_mode_is_chosen_and_then_reported_back)
{
	CHECK_EQ_INT(spdif_is_passthrough_on(), -1);
	spdif_set_passthrough(1);
	CHECK_EQ_INT(spdif_is_passthrough_on(), 1);
	spdif_set_passthrough(2);
	CHECK_EQ_INT(spdif_is_passthrough_on(), 2);
	spdif_set_passthrough(0);
	CHECK_EQ_INT(spdif_is_passthrough_on(), 0);
}

TEST(hdmi_capability_flags_are_stored_as_given)
{
	CHECK_EQ_INT(get_hdmi_supported_audio_codecs(), 0);
	set_hdmi_supported_audio_codecs(0x12345678L);
	CHECK_EQ_INT(get_hdmi_supported_audio_codecs(), 0x12345678L);
	set_hdmi_supported_audio_codecs(0);
	CHECK_EQ_INT(get_hdmi_supported_audio_codecs(), 0);
}

TEST(eight_channel_192khz_iec_is_inferred_from_dts_hd_or_truehd_support)
{
	set_hdmi_supported_audio_codecs(0);
	CHECK_EQ_INT(get_hdmi_supports_iec_8ch192khz(), 0);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS_HD));
	CHECK_EQ_INT(get_hdmi_supports_iec_8ch192khz(), 1);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS_HD_MA));
	CHECK_EQ_INT(get_hdmi_supports_iec_8ch192khz(), 1);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DOLBY_TRUEHD));
	CHECK_EQ_INT(get_hdmi_supports_iec_8ch192khz(), 1);
	/* plain DTS, AC3, E-AC3 and IEC61937 itself say nothing about 8ch/192k */
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS) | bit(ENCODING_AC3) | bit(ENCODING_E_AC3) | bit(ENCODING_IEC61937));
	CHECK_EQ_INT(get_hdmi_supports_iec_8ch192khz(), 0);
}

TEST(iec61937_support_is_its_own_flag_bit)
{
	set_hdmi_supported_audio_codecs(0);
	CHECK_EQ_INT(get_hdmi_supports_iec(), 0);
	set_hdmi_supported_audio_codecs(bit(ENCODING_IEC61937));
	CHECK_EQ_INT(get_hdmi_supports_iec(), 1);
	set_hdmi_supported_audio_codecs(~bit(ENCODING_IEC61937));
	CHECK_EQ_INT(get_hdmi_supports_iec(), 0);
}

TEST(on_a_host_build_every_format_counts_as_passthrough_capable)
{
	/* without CONFIG_ANDROID there is no route to ask: the answer is always yes */
	set_hdmi_supported_audio_codecs(0);
	CHECK_EQ_INT(spdif_format_passthrough_supported(WAVE_FORMAT_AC3), 1);
	CHECK_EQ_INT(spdif_format_passthrough_supported(WAVE_FORMAT_TRUEHD), 1);
	CHECK_EQ_INT(spdif_format_passthrough_supported(0x1234), 1);
}

TEST(dts_hd_formats_and_route_helpers)
{
	CHECK_EQ_INT(spdif_is_dts_hd_format(WAVE_FORMAT_DTS_HD), 1);
	CHECK_EQ_INT(spdif_is_dts_hd_format(WAVE_FORMAT_DTS_HD_MA), 1);
	CHECK_EQ_INT(spdif_is_dts_hd_format(WAVE_FORMAT_DTS), 0);
	CHECK_EQ_INT(spdif_is_dts_hd_format(WAVE_FORMAT_AC3), 0);

	set_hdmi_supported_audio_codecs(0);
	CHECK_EQ_INT(spdif_route_supports_dts_core(), 0);
	CHECK_EQ_INT(spdif_route_supports_dts_hd(), 0);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS));
	CHECK_EQ_INT(spdif_route_supports_dts_core(), 1);
	CHECK_EQ_INT(spdif_route_supports_dts_hd(), 0);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS_HD));
	CHECK_EQ_INT(spdif_route_supports_dts_hd(), 1);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS_HD_MA));
	CHECK_EQ_INT(spdif_route_supports_dts_hd(), 1);
}

TEST(dts_core_fallback_does_not_exist_on_a_host_build_and_the_carrier_follows_the_8ch_flag)
{
	spdif_set_passthrough(1);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS));
	CHECK_EQ_INT(spdif_mode1_dts_core_fallback(WAVE_FORMAT_DTS_HD_MA), 0);
	CHECK_EQ_INT(spdif_mode1_dts_hd_carrier(WAVE_FORMAT_DTS_HD_MA), 0);	/* no 8ch/192k support */
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS_HD_MA));
	CHECK_EQ_INT(spdif_mode1_dts_hd_carrier(WAVE_FORMAT_DTS_HD_MA), 1);
	CHECK_EQ_INT(spdif_mode1_dts_hd_carrier(WAVE_FORMAT_DTS_HD), 1);
	CHECK_EQ_INT(spdif_mode1_dts_hd_carrier(WAVE_FORMAT_DTS), 0);		/* core DTS never uses the HD carrier */
	CHECK_EQ_INT(spdif_mode1_dts_hd_carrier(WAVE_FORMAT_AC3), 0);
}

TEST(spdif_is_only_selected_for_a_forced_mode_1_on_a_host_build)
{
	spdif_set_passthrough(1);
	CHECK_EQ_INT(spdif_check(WAVE_FORMAT_AC3), 1);
	spdif_set_passthrough(2);
	CHECK_EQ_INT(spdif_check(WAVE_FORMAT_AC3), 0);
	spdif_set_passthrough(0);
	CHECK_EQ_INT(spdif_check(WAVE_FORMAT_AC3), 0);
	spdif_set_passthrough(-1);
	CHECK_EQ_INT(spdif_check(WAVE_FORMAT_AC3), 0);
}

/* ---- WAVE format -> libav codec id, gated by what the HDMI sink says it can take --------------- */

struct codec_case { int format; long flags; int expect; };

TEST(codec_ids_are_only_returned_when_the_sink_advertises_a_matching_encoding)
{
	const struct codec_case c[] = {
		/* AC3: either AC3 or E-AC3 support is enough ("some TVs only declare EAC3") */
		{ WAVE_FORMAT_AC3,   bit(ENCODING_AC3),   AV_CODEC_ID_AC3 },
		{ WAVE_FORMAT_AC3,   bit(ENCODING_E_AC3), AV_CODEC_ID_AC3 },
		{ WAVE_FORMAT_AC3,   bit(ENCODING_DTS),   0 },
		{ WAVE_FORMAT_AC3,   0,                   0 },
		/* E-AC3 needs E-AC3 */
		{ WAVE_FORMAT_EAC3,  bit(ENCODING_E_AC3), AV_CODEC_ID_EAC3 },
		{ WAVE_FORMAT_EAC3,  bit(ENCODING_AC3),   0 },
		/* JOC: JOC or plain E-AC3 */
		{ WAVE_FORMAT_E_AC3_JOC, bit(ENCODING_E_AC3_JOC), AV_CODEC_ID_EAC3 },
		{ WAVE_FORMAT_E_AC3_JOC, bit(ENCODING_E_AC3),     AV_CODEC_ID_EAC3 },
		{ WAVE_FORMAT_E_AC3_JOC, bit(ENCODING_AC3),       0 },
		/* the three DTS formats accept any DTS flavour */
		{ WAVE_FORMAT_DTS,       bit(ENCODING_DTS),       AV_CODEC_ID_DTS },
		{ WAVE_FORMAT_DTS,       bit(ENCODING_DTS_HD),    AV_CODEC_ID_DTS },
		{ WAVE_FORMAT_DTS,       bit(ENCODING_DTS_HD_MA), AV_CODEC_ID_DTS },
		{ WAVE_FORMAT_DTS_HD,    bit(ENCODING_DTS),       AV_CODEC_ID_DTS },
		{ WAVE_FORMAT_DTS_HD_MA, bit(ENCODING_DTS_HD_MA), AV_CODEC_ID_DTS },
		{ WAVE_FORMAT_DTS_HD_MA, bit(ENCODING_AC3),       0 },
		/* TrueHD */
		{ WAVE_FORMAT_TRUEHD, bit(ENCODING_DOLBY_TRUEHD), AV_CODEC_ID_TRUEHD },
		{ WAVE_FORMAT_TRUEHD, bit(ENCODING_E_AC3),        0 },
		/* anything else is not a passthrough format */
		{ WAVE_FORMAT_MPEGLAYER3, ~0L, 0 },
		{ 0x7777, ~0L, 0 },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
		set_hdmi_supported_audio_codecs(c[i].flags);
		int got = wave2libav_codecid(c[i].format);
		CHECK_MSG(got == c[i].expect, "format %04X flags %lx: got %d expected %d", c[i].format, c[i].flags, got, c[i].expect);
	}
}

/* ---- logical frame duration ------------------------------------------------------------------- */

static void with_codec_state(int avctx_frame_size, int parser_duration, enum AVCodecID parser_id)
{
	avctx = avcodec_alloc_context3(NULL);
	avctx->frame_size = avctx_frame_size;
	if (parser_duration >= 0) {
		aparser = av_parser_init(parser_id);
		REQUIRE_NOT_NULL(aparser);
		aparser->duration = parser_duration;
	}
}

static void drop_codec_state(void)
{
	if (aparser) av_parser_close(aparser);
	aparser = NULL;
	if (avctx) avcodec_free_context(&avctx);
	avctx = NULL;
}

TEST(frame_samples_prefer_the_codec_context_then_the_parser_then_the_format_default)
{
	AUDIO_PROPERTIES ac3, eac3, joc, thd, dts, pcm;
	props(&ac3, WAVE_FORMAT_AC3, 48000, 2);
	props(&eac3, WAVE_FORMAT_EAC3, 48000, 2);
	props(&joc, WAVE_FORMAT_E_AC3_JOC, 48000, 2);
	props(&thd, WAVE_FORMAT_TRUEHD, 48000, 2);
	props(&dts, WAVE_FORMAT_DTS, 48000, 2);
	props(&pcm, WAVE_FORMAT_MPEGLAYER3, 48000, 2);

	/* nothing known: format defaults */
	CHECK_EQ_INT(_spdif_frame_samples(&ac3, 1), 1536);
	CHECK_EQ_INT(_spdif_frame_samples(&eac3, 1), 1536);
	CHECK_EQ_INT(_spdif_frame_samples(&joc, 1), 1536);
	CHECK_EQ_INT(_spdif_frame_samples(&thd, 1), 1280);
	CHECK_EQ_INT(_spdif_frame_samples(&dts, 1), 0);
	CHECK_EQ_INT(_spdif_frame_samples(&pcm, 1), 0);
	CHECK_EQ_INT(_spdif_frame_samples(NULL, 1), 0);

	/* parser duration beats the defaults */
	with_codec_state(0, 640, AV_CODEC_ID_AC3);
	CHECK_EQ_INT(_spdif_frame_samples(&ac3, 1), 640);
	CHECK_EQ_INT(_spdif_frame_samples(NULL, 1), 640);
	drop_codec_state();

	/* a parser that knows nothing (duration 0) falls through to the defaults */
	with_codec_state(0, 0, AV_CODEC_ID_AC3);
	CHECK_EQ_INT(_spdif_frame_samples(&ac3, 1), 1536);
	drop_codec_state();

	/* codec context frame size beats the parser */
	with_codec_state(1024, 640, AV_CODEC_ID_AC3);
	CHECK_EQ_INT(_spdif_frame_samples(&ac3, 1), 1024);
	CHECK_EQ_INT(_spdif_frame_samples(&thd, 1), 1024);
	drop_codec_state();
}

TEST(dts_in_mode_2_follows_the_parser_then_the_codec_context_then_512)
{
	AUDIO_PROPERTIES dts, dtshd, dtsma;
	props(&dts, WAVE_FORMAT_DTS, 48000, 2);
	props(&dtshd, WAVE_FORMAT_DTS_HD, 48000, 2);
	props(&dtsma, WAVE_FORMAT_DTS_HD_MA, 48000, 2);

	CHECK_EQ_INT(_spdif_frame_samples(&dts, 2), 512);
	CHECK_EQ_INT(_spdif_frame_samples(&dtshd, 2), 512);
	CHECK_EQ_INT(_spdif_frame_samples(&dtsma, 2), 512);

	with_codec_state(1024, 0, AV_CODEC_ID_DTS);		/* parser has no duration */
	CHECK_EQ_INT(_spdif_frame_samples(&dts, 2), 1024);
	drop_codec_state();

	with_codec_state(1024, 2048, AV_CODEC_ID_DTS);		/* parser wins */
	CHECK_EQ_INT(_spdif_frame_samples(&dts, 2), 2048);
	CHECK_EQ_INT(_spdif_frame_samples(&dts, 1), 1024);	/* in mode 1 the context wins instead */
	drop_codec_state();
}

TEST(the_debug_variant_reports_the_same_duration_and_names_where_it_came_from)
{
	AUDIO_PROPERTIES ac3, thd, dts, pcm;
	props(&ac3, WAVE_FORMAT_AC3, 48000, 2);
	props(&thd, WAVE_FORMAT_TRUEHD, 48000, 2);
	props(&dts, WAVE_FORMAT_DTS, 48000, 2);
	props(&pcm, WAVE_FORMAT_MPEGLAYER3, 48000, 2);
	int a = -1, p = -1, f = -1;
	const char *src = NULL;

	CHECK_EQ_INT(_spdif_frame_samples_debug(&ac3, 1, &a, &p, &f, &src), 1536);
	CHECK_EQ_STR(src, "fallback-ac3");
	CHECK(a == 0 && p == 0 && f == 1536);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&thd, 1, &a, &p, &f, &src), 1280);
	CHECK_EQ_STR(src, "fallback-truehd");
	CHECK_EQ_INT(f, 1280);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&pcm, 1, &a, &p, &f, &src), 0);
	CHECK_EQ_STR(src, "none");
	CHECK_EQ_INT(_spdif_frame_samples_debug(NULL, 1, &a, &p, &f, NULL), 0);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&dts, 2, &a, &p, &f, &src), 512);
	CHECK_EQ_STR(src, "fallback-dts");
	CHECK_EQ_INT(f, 512);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&dts, 2, &a, &p, &f, NULL), 512);

	with_codec_state(0, 640, AV_CODEC_ID_AC3);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&ac3, 1, &a, &p, &f, &src), 640);
	CHECK_EQ_STR(src, "parser");
	CHECK(p == 640 && a == 0);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&ac3, 1, &a, &p, &f, NULL), 640);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&dts, 2, &a, &p, &f, &src), 640);
	CHECK_EQ_STR(src, "parser");
	CHECK_EQ_INT(_spdif_frame_samples_debug(&dts, 2, &a, &p, &f, NULL), 640);
	drop_codec_state();

	with_codec_state(1024, 640, AV_CODEC_ID_AC3);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&ac3, 1, &a, &p, &f, &src), 1024);
	CHECK_EQ_STR(src, "avctx");
	CHECK(a == 1024 && p == 0);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&ac3, 1, &a, &p, &f, NULL), 1024);
	drop_codec_state();

	with_codec_state(1024, 0, AV_CODEC_ID_DTS);
	CHECK_EQ_INT(_spdif_frame_samples_debug(&dts, 2, &a, &p, &f, &src), 1024);
	CHECK_EQ_STR(src, "avctx");
	CHECK_EQ_INT(_spdif_frame_samples_debug(&dts, 2, &a, &p, &f, NULL), 1024);
	drop_codec_state();
}

/* ---- spdif_init ---------------------------------------------------------------------------------- */

TEST(init_builds_muxer_codec_context_parser_and_packet_for_each_passthrough_format)
{
	static const int formats[] = { WAVE_FORMAT_AC3, WAVE_FORMAT_EAC3, WAVE_FORMAT_E_AC3_JOC, WAVE_FORMAT_DTS,
				       WAVE_FORMAT_DTS_HD, WAVE_FORMAT_DTS_HD_MA, WAVE_FORMAT_TRUEHD };
	for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		AUDIO_PROPERTIES a;
		props(&a, formats[i], 48000, 2);
		set_hdmi_supported_audio_codecs(~0L);
		spdif_set_passthrough(1);
		int ok = spdif_init(&a);
		CHECK_MSG(ok == 1, "format %04X: init %d", formats[i], ok);
		CHECK_MSG(fctxt && avctx && aparser && spdif_pkt, "format %04X: missing %p %p %p %p", formats[i], (void *)fctxt, (void *)avctx, (void *)aparser, (void *)spdif_pkt);
		spdif_close(&a);
		CHECK_MSG(!fctxt && !avctx && !aparser && !spdif_pkt, "format %04X: close left state behind", formats[i]);
	}
}

TEST(init_returns_zero_without_touching_ffmpeg_when_mode_1_is_not_selected)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 2);
	set_hdmi_supported_audio_codecs(~0L);
	spdif_set_passthrough(0);
	CHECK_EQ_INT(spdif_init(&a), 0);
	CHECK_NULL(fctxt);
	spdif_set_passthrough(2);
	CHECK_EQ_INT(spdif_init(&a), 0);			/* host build: only mode 1 is IEC wrapping */
	CHECK_NULL(fctxt);
}

TEST(init_fails_cleanly_when_the_sink_cannot_take_the_codec)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 2);
	set_hdmi_supported_audio_codecs(0);		/* wave2libav_codecid() answers "none" */
	spdif_set_passthrough(1);
	CHECK_EQ_INT(spdif_init(&a), 0);
	CHECK(!fctxt && !avctx && !aparser && !spdif_pkt);	/* the failure path released everything */
	props(&a, WAVE_FORMAT_MPEGLAYER3, 48000, 2);	/* not a passthrough format at all */
	set_hdmi_supported_audio_codecs(~0L);
	CHECK_EQ_INT(spdif_init(&a), 0);
	CHECK(!fctxt && !avctx && !aparser && !spdif_pkt);
}

TEST(re_initialising_replaces_the_previous_session)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 2);
	set_hdmi_supported_audio_codecs(~0L);
	spdif_set_passthrough(1);
	CHECK_EQ_INT(spdif_init(&a), 1);
	AVFormatContext *first = fctxt;
	b.pos = 123;					/* stale data from the old session must go */
	CHECK_EQ_INT(spdif_init(&a), 1);
	CHECK(fctxt != NULL);
	(void)first;
	CHECK_EQ_INT(b.pos, 0);
	CHECK_EQ_INT(spdif_close(&a), 0);
	CHECK_EQ_INT(spdif_close(&a), 0);		/* closing twice is harmless */
}

TEST(init_takes_the_codec_sample_rate_from_the_source_when_known)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 2);
	a.sourceSamples = 44100;
	set_hdmi_supported_audio_codecs(~0L);
	spdif_set_passthrough(1);
	REQUIRE(spdif_init(&a) == 1);
	CHECK_EQ_INT(avctx->sample_rate, 44100);
	spdif_close(&a);

	a.sourceSamples = 0;
	REQUIRE(spdif_init(&a) == 1);
	CHECK_EQ_INT(avctx->sample_rate, 48000);
	spdif_close(&a);
}

/* ---- spdif_open: geometry of the carrier ---------------------------------------------------------- */

struct geom { int format; int rate; long flags; int ch, sps; };

TEST(open_in_mode_1_describes_the_iec_carrier_for_every_format)
{
	const struct geom g[] = {
		/* AC3 and DTS: 16 bit stereo, 48 kHz unless the source is 32 or 44.1 kHz */
		{ WAVE_FORMAT_AC3, 48000, ~0L, 2, 48000 }, { WAVE_FORMAT_AC3, 44100, ~0L, 2, 44100 },
		{ WAVE_FORMAT_AC3, 32000, ~0L, 2, 32000 }, { WAVE_FORMAT_AC3, 96000, ~0L, 2, 48000 },
		{ WAVE_FORMAT_DTS, 48000, ~0L, 2, 48000 }, { WAVE_FORMAT_DTS, 44100, ~0L, 2, 44100 },
		{ 0x7777,          44100, ~0L, 2, 44100 },	/* default branch */
		/* E-AC3 carries 1536 samples in a 4x burst */
		{ WAVE_FORMAT_EAC3, 48000, ~0L, 2, 192000 }, { WAVE_FORMAT_E_AC3_JOC, 44100, ~0L, 2, 176400 },
		/* DTS-HD with a high bitrate carrier */
		{ WAVE_FORMAT_DTS_HD, 48000, ~0L, 8, 192000 }, { WAVE_FORMAT_DTS_HD_MA, 48000, ~0L, 8, 192000 },
		/* TrueHD: 192 kHz family or 176.4 kHz family */
		{ WAVE_FORMAT_TRUEHD, 48000, ~0L, 8, 192000 }, { WAVE_FORMAT_TRUEHD, 96000, ~0L, 8, 192000 },
		{ WAVE_FORMAT_TRUEHD, 44100, ~0L, 8, 176400 }, { WAVE_FORMAT_TRUEHD, 88200, ~0L, 8, 176400 },
	};
	for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) {
		AUDIO_PROPERTIES a;
		props(&a, g[i].format, g[i].rate, 6);
		spdif_set_passthrough(1);
		set_hdmi_supported_audio_codecs(g[i].flags);
		CHECK_EQ_INT(spdif_open(&a), 0);
		CHECK_MSG(a.channels == g[i].ch && a.samplesPerSec == g[i].sps,
			  "case %zu (format %04X @%d): %d ch %d Hz, expected %d ch %d Hz", i, g[i].format, g[i].rate, a.channels, a.samplesPerSec, g[i].ch, g[i].sps);
		CHECK_MSG(a.bitsPerSample == 16, "case %zu: bits %d", i, a.bitsPerSample);
		CHECK_MSG(a.bytesPerFrame == a.channels * 2, "case %zu: bpf %d", i, a.bytesPerFrame);
		CHECK_MSG(a.bytesPerSec == a.samplesPerSec * a.bytesPerFrame, "case %zu: Bps %d", i, a.bytesPerSec);
		drop_codec_state();
	}
}

TEST(open_in_mode_1_for_dts_hd_without_8ch_support_falls_back_to_the_dts_core_container)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_DTS_HD_MA, 48000, 8);
	spdif_set_passthrough(1);
	set_hdmi_supported_audio_codecs(bit(ENCODING_DTS));
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_EQ_INT(a.channels, 2);
	CHECK_EQ_INT(a.samplesPerSec, 48000);
	drop_codec_state();

	props(&a, WAVE_FORMAT_DTS_HD, 44100, 8);
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_EQ_INT(a.samplesPerSec, 44100);
	drop_codec_state();
	props(&a, WAVE_FORMAT_DTS_HD, 32000, 8);
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_EQ_INT(a.samplesPerSec, 32000);
	drop_codec_state();
	props(&a, WAVE_FORMAT_DTS_HD, 96000, 8);
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_EQ_INT(a.samplesPerSec, 48000);
	drop_codec_state();
}

TEST(open_remembers_the_source_layout_before_changing_it)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	spdif_set_passthrough(1);
	spdif_open(&a);
	CHECK_EQ_INT(a.sourceSamples, 48000);
	CHECK_EQ_INT(a.sourceChannels, 6);
	CHECK_EQ_INT(a.sourceBitsPerSample, 16);
	CHECK_EQ_INT(a.channels, 2);			/* but the sink sees the carrier */
	drop_codec_state();

	props(&a, WAVE_FORMAT_AC3, 44100, 6);
	a.sourceSamples = 48000;			/* already set by an earlier open: kept */
	a.sourceChannels = 8;
	a.sourceBitsPerSample = 24;
	spdif_open(&a);
	CHECK_EQ_INT(a.sourceSamples, 48000);
	CHECK_EQ_INT(a.sourceChannels, 8);
	CHECK_EQ_INT(a.sourceBitsPerSample, 24);
	drop_codec_state();
}

TEST(open_in_mode_2_uses_synthetic_stereo_16_bit_geometry_at_the_source_rate)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_EAC3, 48000, 6);
	spdif_set_passthrough(2);
	set_hdmi_supported_audio_codecs(~0L);
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_EQ_INT(a.channels, 2);
	CHECK_EQ_INT(a.bitsPerSample, 16);
	CHECK_EQ_INT(a.samplesPerSec, 48000);		/* not the 192 kHz IEC carrier */
	CHECK_EQ_INT(a.bytesPerFrame, 4);
	CHECK_EQ_INT(a.bytesPerSec, 192000);
	CHECK_EQ_INT(a.sourceChannels, 6);
	drop_codec_state();
}

TEST(open_leaves_the_properties_alone_when_passthrough_is_off)
{
	AUDIO_PROPERTIES a, before;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	before = a;
	spdif_set_passthrough(0);
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_EQ_INT(a.channels, before.channels);
	CHECK_EQ_INT(a.samplesPerSec, before.samplesPerSec);
	CHECK_EQ_INT(a.bytesPerFrame, before.bytesPerFrame);
	drop_codec_state();
}

TEST(open_creates_a_parser_and_context_when_init_did_not)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	set_hdmi_supported_audio_codecs(~0L);
	spdif_set_passthrough(1);
	CHECK_NULL(aparser);
	spdif_open(&a);
	CHECK_NOT_NULL(aparser);
	CHECK_NOT_NULL(avctx);
	CHECK_EQ_INT(avctx->sample_rate, 48000);
	CHECK_EQ_INT(avctx->ch_layout.nb_channels, 6);
	drop_codec_state();
}

TEST(open_without_a_usable_codec_creates_no_parser)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	set_hdmi_supported_audio_codecs(0);		/* codec id 0: no parser exists */
	spdif_set_passthrough(1);
	CHECK_EQ_INT(spdif_open(&a), 0);
	CHECK_NULL(aparser);
	drop_codec_state();
}

TEST(open_keeps_the_parser_and_context_that_init_built)
{
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	AVCodecParserContext *p = aparser;
	AVCodecContext *c = avctx;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	spdif_open(&a);
	CHECK(aparser == p);
	CHECK(avctx == c);
	spdif_close(&a);
}

/* ---- encapsulation of real compressed audio ------------------------------------------------------- */

TEST(ac3_frames_are_wrapped_into_iec61937_bursts_that_ffmpegs_own_demuxer_unwraps_losslessly)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 12) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	Run r;
	run_packets(&a, &cs, 1, &r);

	CHECK_EQ_INT(r.fatal, 0);
	CHECK_EQ_INT(r.decoded_total, cs.size);			/* everything offered was consumed */
	CHECK_EQ_INT(r.out_size, cs.npkts * 6144);		/* one 6144-byte (1536 stereo 16-bit) burst per AC3 frame */
	CHECK_EQ_INT(r.calls_with_output, cs.npkts);
	CHECK_EQ_INT(r.first_fake, 1536 * a.bytesPerFrame);	/* logical duration of one AC3 frame */

	Demuxed d;
	REQUIRE(iec_demux(r.out, r.out_size, &d) == 0);
	CHECK_EQ_INT(d.codec, AV_CODEC_ID_AC3);
	CHECK_EQ_INT(d.npkts, cs.npkts);
	CHECK_EQ_INT(d.size, cs.size);
	CHECK_EQ_MEM(d.payload, cs.data, cs.size);
	free(d.payload); free(r.out); coded_free(&cs);
	spdif_close(&a);
}

TEST(burst_headers_carry_the_iec61937_preamble_and_the_payload_length_in_bits)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 4) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	Run r;
	run_packets(&a, &cs, 1, &r);
	REQUIRE(r.out_size >= 6144);
	const unsigned char *o = r.out;
	CHECK_EQ_INT(o[0] | (o[1] << 8), 0xF872);		/* Pa */
	CHECK_EQ_INT(o[2] | (o[3] << 8), 0x4E1F);		/* Pb */
	CHECK_EQ_INT((o[4] | (o[5] << 8)) & 0x1F, 1);		/* Pc: data type 1 = AC-3 */
	CHECK_EQ_INT(o[6] | (o[7] << 8), cs.pkt_size[0] * 8);	/* Pd: length in bits */
	free(r.out); coded_free(&cs);
	spdif_close(&a);
}

TEST(eac3_frames_travel_in_four_times_larger_bursts)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "eac3", 48000, 192000, 10) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_EAC3, 48000, 6, ~0L);
	CHECK_EQ_INT(a.samplesPerSec, 192000);
	Run r;
	run_packets(&a, &cs, 1, &r);
	CHECK_EQ_INT(r.fatal, 0);
	CHECK_EQ_INT(r.out_size, cs.npkts * 24576);
	Demuxed d;
	REQUIRE(iec_demux(r.out, r.out_size, &d) == 0);
	CHECK_EQ_INT(d.codec, AV_CODEC_ID_EAC3);
	CHECK_EQ_INT(d.size, cs.size);
	CHECK_EQ_MEM(d.payload, cs.data, cs.size);
	free(d.payload); free(r.out); coded_free(&cs);
	spdif_close(&a);
}

TEST(dts_frames_are_wrapped_in_bursts_sized_by_the_frame_duration)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "dca", 48000, 1411200, 10) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_DTS, 48000, 6, ~0L);
	Run r;
	run_packets(&a, &cs, 1, &r);
	CHECK_EQ_INT(r.fatal, 0);
	CHECK_EQ_INT(r.out_size, cs.npkts * 2048);		/* 512 samples * 4 bytes */
	Demuxed d;
	REQUIRE(iec_demux(r.out, r.out_size, &d) == 0);
	CHECK_EQ_INT(d.codec, AV_CODEC_ID_DTS);
	CHECK_EQ_INT(d.npkts, cs.npkts);
	free(d.payload); free(r.out); coded_free(&cs);
	spdif_close(&a);
}

TEST(a_real_ac3_elementary_stream_cut_at_arbitrary_boundaries_is_reassembled_by_the_parser)
{
	int size;
	unsigned char *file = slurp("media/sine.ac3", &size);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 44100, 2, ~0L);	/* the fixture is 44.1 kHz */

	unsigned char *out = NULL;
	int out_size = 0, decoded_total = 0;
	const int chunk = 1000;					/* does not align with frame boundaries */
	for (int off = 0; off <= size; ) {
		int n = off + chunk <= size ? chunk : size - off;
		AUDIO_FRAME fr;
		memset(&fr, 0, sizeof fr);
		int decoded = 0;
		CHECK_EQ_INT(spdif_encapsulate(&a, file + off, n, &fr, &decoded), 0);
		CHECK(decoded >= 0 && decoded <= n);
		if (fr.size) {
			out = realloc(out, out_size + fr.size);
			memcpy(out + out_size, fr.data, fr.size);
			out_size += fr.size;
		}
		/* the parser may consume less than offered: advance by what it took */
		if (n == 0) break;
		off += decoded ? decoded : n;
		decoded_total += decoded;
	}
	CHECK(out_size > 0);

	Demuxed d;
	REQUIRE(iec_demux(out, out_size, &d) == 0);
	CHECK_EQ_INT(d.codec, AV_CODEC_ID_AC3);
	CHECK(d.npkts >= 20);
	CHECK(d.size <= size);
	CHECK_EQ_MEM(d.payload, file, d.size);			/* every frame comes back bit-exact, in order */
	free(d.payload); free(out); free(file);
	spdif_close(&a);
}

TEST(draining_at_eof_flushes_the_last_buffered_frame)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 3) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	Run without, with;
	run_packets(&a, &cs, 0, &without);
	spdif_close(&a);
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	run_packets(&a, &cs, 1, &with);
	CHECK_EQ_INT(without.out_size, (cs.npkts - 1) * 6144);	/* the parser holds the last frame back */
	CHECK_EQ_INT(with.out_size, cs.npkts * 6144);
	free(without.out); free(with.out); coded_free(&cs);
	spdif_close(&a);
}

TEST(a_batch_of_frames_with_a_matching_frame_count_is_accepted_and_an_inconsistent_count_is_reset_to_one)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 6) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);

	AUDIO_FRAME fr;
	int decoded;
	/* two packets in one call with frame_count 2 */
	memset(&fr, 0, sizeof fr);
	CHECK_EQ_INT(spdif_encapsulate_frames(&a, cs.data, cs.pkt_size[0] * 2, &fr, &decoded, 2), 0);
	CHECK(decoded > 0);
	/* frame_count that does not divide the size, or is < 1, behaves like 1 */
	memset(&fr, 0, sizeof fr);
	CHECK_EQ_INT(spdif_encapsulate_frames(&a, cs.data, cs.pkt_size[0] + 1, &fr, &decoded, 2), 0);
	memset(&fr, 0, sizeof fr);
	CHECK_EQ_INT(spdif_encapsulate_frames(&a, cs.data, cs.pkt_size[0], &fr, &decoded, 0), 0);
	memset(&fr, 0, sizeof fr);
	CHECK_EQ_INT(spdif_encapsulate_frames(&a, cs.data, cs.pkt_size[0], &fr, &decoded, -3), 0);
	coded_free(&cs);
	spdif_close(&a);
}

TEST(encapsulating_before_init_is_a_fatal_error)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 2);
	unsigned char data[64] = { 0 };
	AUDIO_FRAME fr;
	memset(&fr, 0, sizeof fr);
	int decoded = 5;
	CHECK_EQ_INT(spdif_encapsulate(&a, data, sizeof data, &fr, &decoded), -1);
	CHECK_EQ_INT(fr.error, STREAM_ERROR_FATAL);
	CHECK_EQ_INT(decoded, 0);
	CHECK_EQ_INT(fr.size, 0);
	CHECK_EQ_INT(fr.fakeSize, 0);
}

TEST(a_zero_sized_input_without_a_parser_has_nothing_to_drain)
{
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	av_parser_close(aparser);
	aparser = NULL;
	AUDIO_FRAME fr;
	memset(&fr, 0, sizeof fr);
	int decoded = 9;
	CHECK_EQ_INT(spdif_encapsulate(&a, NULL, 0, &fr, &decoded), 0);
	CHECK_EQ_INT(decoded, 0);
	CHECK_EQ_INT(fr.size, 0);
	spdif_close(&a);
}

TEST(without_a_parser_a_complete_frame_is_wrapped_directly)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 4) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	av_parser_close(aparser);
	aparser = NULL;				/* the situation of an encoder that already emits whole syncframes */

	AUDIO_FRAME fr;
	memset(&fr, 0, sizeof fr);
	int decoded = 0;
	CHECK_EQ_INT(spdif_encapsulate(&a, cs.data, cs.pkt_size[0], &fr, &decoded), 0);
	CHECK_EQ_INT(decoded, cs.pkt_size[0]);
	CHECK_EQ_INT(fr.size, 6144);
	CHECK_EQ_INT(fr.error, 0);
	CHECK_EQ_INT(fr.format, WAVE_FORMAT_UNKNOWN);
	Demuxed d;
	REQUIRE(iec_demux(fr.data, fr.size, &d) == 0);
	CHECK_EQ_INT(d.size, cs.pkt_size[0]);
	CHECK_EQ_MEM(d.payload, cs.data, cs.pkt_size[0]);
	free(d.payload); coded_free(&cs);
	spdif_close(&a);
}

TEST(data_the_muxer_refuses_is_reported_as_a_fatal_error)
{
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	av_parser_close(aparser);
	aparser = NULL;
	static unsigned char junk[8000];
	memset(junk, 0x55, sizeof junk);	/* larger than the 6144-byte IEC burst: the muxer must refuse it */
	AUDIO_FRAME fr;
	memset(&fr, 0, sizeof fr);
	int decoded = 0;
	CHECK_EQ_INT(spdif_encapsulate(&a, junk, sizeof junk, &fr, &decoded), -1);
	CHECK_EQ_INT(fr.error, STREAM_ERROR_FATAL);
	spdif_close(&a);
}

TEST(a_payload_shorter_than_ten_bytes_is_ignored_by_the_muxer_wrapper)
{
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	int decoded = 77;
	CHECK_EQ_INT(spdif_put((UCHAR *)"123456789", 9, &decoded), 0);
	CHECK_EQ_INT(decoded, 77);			/* untouched: nothing was consumed */
	CHECK_EQ_INT(spdif_put(NULL, 100, &decoded), 0);
	CHECK_EQ_INT(b.pos, 0);
	spdif_close(&a);
}

TEST(put_without_a_session_returns_zero_instead_of_crashing)
{
	unsigned char data[64] = { 0 };
	int decoded = 0;
	CHECK_EQ_INT(spdif_put(data, sizeof data, &decoded), 0);
}

TEST(output_larger_than_the_collection_buffer_is_an_error_and_discards_the_partial_data)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 3) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	av_parser_close(aparser);
	aparser = NULL;
	/* the sink never calls spdif_get here: 6144-byte bursts pile up until 64 KiB are exceeded */
	int failed_at = -1;
	for (int i = 0; i < 20; i++) {
		int decoded = 0;
		if (spdif_put(cs.data, cs.pkt_size[0], &decoded) < 0) {
			failed_at = i;
			break;
		}
	}
	CHECK(failed_at >= 10);				/* 10 bursts = 61440 bytes still fit, the 11th does not */
	CHECK_EQ_INT(b.pos, 0);				/* partial data dropped */
	coded_free(&cs);
	spdif_close(&a);
}

TEST(get_hands_over_the_collected_bytes_once)
{
	AUDIO_FRAME fr;
	memset(&fr, 0xAA, sizeof fr);
	b.pos = 0;
	CHECK_EQ_INT(spdif_get(&fr), 0);
	CHECK_EQ_INT(fr.size, 0);
	CHECK_EQ_INT(fr.error, 0);
	CHECK_EQ_INT(fr.format, WAVE_FORMAT_UNKNOWN);

	memcpy(b.buf, "payload", 7);
	b.pos = 7;
	CHECK_EQ_INT(spdif_get(&fr), 0);
	CHECK_EQ_INT(fr.size, 7);
	CHECK_EQ_INT(fr.fakeSize, 7);
	CHECK_EQ_MEM(fr.data, "payload", 7);
	CHECK_EQ_INT(b.pos, 0);				/* consumed */
	CHECK_EQ_INT(spdif_get(&fr), 0);
	CHECK_EQ_INT(fr.size, 0);
}

/* ---- mode 2: raw codec frames to the sink ---------------------------------------------------------- */

static void switch_to_mode2(AUDIO_PROPERTIES *a)
{
	spdif_set_passthrough(2);
	a->channels = 2;				/* what open() hands the sink in mode 2 */
	a->bytesPerFrame = 4;
	a->bytesPerSec = a->samplesPerSec * 4;
}

TEST(mode_2_passes_the_parsers_raw_frames_through_untouched_with_the_codec_id_and_duration)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 12) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);	/* init needs mode 1 on a host build */
	switch_to_mode2(&a);

	Run r;
	run_packets(&a, &cs, 1, &r);
	CHECK_EQ_INT(r.fatal, 0);
	CHECK_EQ_INT(r.out_size, cs.size);			/* the original bytes, not IEC bursts */
	CHECK_EQ_MEM(r.out, cs.data, cs.size);
	CHECK_EQ_INT(r.calls_with_output, cs.npkts);		/* one raw frame per AC3 frame */
	CHECK_EQ_INT(r.last_format, WAVE_FORMAT_AC3);		/* codec id preserved for the sink */
	CHECK_EQ_INT(r.first_fake, 1536 * a.bytesPerFrame);
	CHECK_EQ_INT(r.fake_total, (long long)cs.npkts * 1536 * a.bytesPerFrame);
	unsigned char *out = r.out;
	free(out); coded_free(&cs);
	spdif_close(&a);
}

TEST(mode_2_dts_uses_the_parsers_512_sample_duration_not_1536)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "dca", 48000, 1411200, 8) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_DTS, 48000, 6, ~0L);
	switch_to_mode2(&a);
	Run r;
	run_packets(&a, &cs, 1, &r);
	CHECK_EQ_INT(r.out_size, cs.size);
	CHECK_EQ_MEM(r.out, cs.data, cs.size);
	CHECK_EQ_INT(r.first_fake, 512 * 4);			/* 1536 would run audio time 3x too fast */
	free(r.out); coded_free(&cs);
	spdif_close(&a);
}

TEST(mode_2_without_parser_output_returns_an_empty_frame)
{
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	switch_to_mode2(&a);
	unsigned char junk[64];
	memset(junk, 0, sizeof junk);			/* the parser finds no sync word: no output */
	AUDIO_FRAME fr;
	memset(&fr, 0, sizeof fr);
	fr.size = 55;
	int decoded = -1;
	CHECK_EQ_INT(spdif_encapsulate(&a, junk, sizeof junk, &fr, &decoded), 0);
	CHECK_EQ_INT(fr.size, 0);
	CHECK_EQ_INT(fr.error, 0);
	spdif_close(&a);
}

TEST(mode_2_duration_for_an_unknown_format_comes_from_the_parser)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 4) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	switch_to_mode2(&a);
	a.format = 0x7777;				/* no format default; context frame_size is 0 */
	Run r;
	run_packets(&a, &cs, 1, &r);
	CHECK(r.out_size > 0);
	CHECK_EQ_INT(r.last_format, 0x7777);
	CHECK_EQ_INT(r.first_fake, 1536 * 4);		/* the AC3 parser's own duration, since the format has no default */
	free(r.out); coded_free(&cs);
	spdif_close(&a);
}

/* ---- the decoder interface and its registration ------------------------------------------------------ */

TEST(the_decoder_callbacks_are_neutral_and_report_what_they_were_asked)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 2);
	int decoded = 5, t = 6;
	AUDIO_FRAME fr;
	memset(&fr, 0, sizeof fr);
	unsigned char d[4] = { 0 };

	CHECK_EQ_INT(stream_spdif.new(&a), 0);
	CHECK_EQ_INT(stream_spdif.new(NULL), 1);
	CHECK_EQ_INT(stream_spdif.decode(&a, d, 4, &fr, &decoded, &t), 0);
	CHECK_EQ_INT(decoded, 0);				/* the decoder never decodes: passthrough only */
	CHECK_EQ_INT(stream_spdif.delay(&a), 0);
	CHECK_EQ_INT(stream_spdif.delete(&a), 0);

	STREAM_RC rc;
	memset(&rc, 0xAB, sizeof rc);
	CHECK_EQ_INT(stream_spdif.get_rc(&a, &rc), 0);
	STREAM_RC zero;
	memset(&zero, 0, sizeof zero);
	CHECK_EQ_MEM(&rc, &zero, sizeof rc);			/* a cleared rate-control record */
	CHECK_EQ_INT(stream_spdif.get_rc(&a, NULL), 1);
}

TEST(close_releases_the_session_and_flush_rebuilds_it)
{
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	AUDIO_PROPERTIES keep = a;
	CHECK_EQ_INT(stream_spdif.close(&a), 0);
	CHECK(!fctxt && !avctx && !aparser && !spdif_pkt);

	set_hdmi_supported_audio_codecs(~0L);
	props(&keep, WAVE_FORMAT_AC3, 48000, 6);
	CHECK_EQ_INT(stream_spdif.flush(&keep), 0);		/* flush == init again */
	CHECK(fctxt && avctx && aparser);
	spdif_close(&keep);

	spdif_set_passthrough(0);
	CHECK_EQ_INT(stream_spdif.flush(&keep), 1);		/* reset impossible: reported */
}

TEST(flush_discards_the_parsers_buffered_look_ahead)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 6) == 0);
	AUDIO_PROPERTIES a;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	AUDIO_FRAME fr;
	int decoded;
	memset(&fr, 0, sizeof fr);
	spdif_encapsulate(&a, cs.data, cs.pkt_size[0], &fr, &decoded);	/* frame 0 is now held by the parser */
	CHECK_EQ_INT(stream_spdif.flush(&a), 0);
	memset(&fr, 0, sizeof fr);
	CHECK_EQ_INT(spdif_encapsulate(&a, NULL, 0, &fr, &decoded), 0);	/* nothing left to drain */
	CHECK_EQ_INT(fr.size, 0);
	coded_free(&cs);
	spdif_close(&a);
}

TEST(support_check_follows_the_selected_mode)
{
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	spdif_set_passthrough(1);
	CHECK_EQ_INT(stream_spdif.is_supported(&a), 1);
	spdif_set_passthrough(0);
	CHECK_EQ_INT(stream_spdif.is_supported(&a), 0);
}

TEST(the_spdif_decoder_is_registered_for_every_passthrough_format_ahead_of_the_software_decoders)
{
	static const int formats[] = { WAVE_FORMAT_AC3, WAVE_FORMAT_EAC3, WAVE_FORMAT_E_AC3_JOC, WAVE_FORMAT_DTS,
				       WAVE_FORMAT_DTS_HD, WAVE_FORMAT_DTS_HD_MA, WAVE_FORMAT_TRUEHD };
	spdif_set_passthrough(1);
	for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		AUDIO_PROPERTIES a;
		props(&a, formats[i], 48000, 6);
		STREAM_DEC_AUDIO *dec = stream_get_audio_dec(&a);
		CHECK_MSG(dec != NULL, "format %04X: no decoder", formats[i]);
		CHECK_MSG(dec && !strcmp(dec->name, "spdif"), "format %04X: first decoder is %s", formats[i], dec ? dec->name : "(null)");
	}
	spdif_set_passthrough(0);			/* with passthrough off the registry must not hand out spdif */
	AUDIO_PROPERTIES a;
	props(&a, WAVE_FORMAT_AC3, 48000, 6);
	STREAM_DEC_AUDIO *dec = stream_get_audio_dec(&a);
	CHECK(!dec || strcmp(dec->name, "spdif") != 0);
}

/* ---- logging and the debug parameter -------------------------------------------------------------------- */

TEST(verbose_logging_does_not_change_what_the_passthrough_produces)
{
	CodedStream cs;
	REQUIRE(coded_encode(&cs, "ac3", 48000, 192000, 8) == 0);
	Run quiet, loud1, loud2;
	AUDIO_PROPERTIES a;

	Debug[DBG_CA] = 0; Debug[DBG_STREAM] = 0;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	run_packets(&a, &cs, 1, &quiet);
	spdif_close(&a);

	Debug[DBG_CA] = 2; Debug[DBG_STREAM] = 1;
	setup_mode1(&a, WAVE_FORMAT_AC3, 48000, 6, ~0L);
	run_packets(&a, &cs, 1, &loud1);
	switch_to_mode2(&a);
	run_packets(&a, &cs, 1, &loud2);
	spdif_close(&a);
	Debug[DBG_CA] = 0; Debug[DBG_STREAM] = 0;

	CHECK_EQ_INT(loud1.out_size, quiet.out_size);
	CHECK_EQ_MEM(loud1.out, quiet.out, quiet.out_size);
	CHECK_EQ_INT(loud2.out_size, cs.size);
	free(quiet.out); free(loud1.out); free(loud2.out); coded_free(&cs);
}

TEST(the_passthrough_on_debug_parameter_sets_the_mode)
{
	spdif_set_passthrough(0);
	debug_do_cmd((const unsigned char *)"passthrough_on 2");
	CHECK_EQ_INT(spdif_is_passthrough_on(), 2);
	debug_do_cmd((const unsigned char *)"passthrough_on");
	CHECK_EQ_INT(spdif_is_passthrough_on(), 2);		/* no argument: only reports */
	debug_do_cmd((const unsigned char *)"passthrough_on 1");
	CHECK_EQ_INT(spdif_is_passthrough_on(), 1);
}
