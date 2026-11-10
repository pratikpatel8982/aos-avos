/*
 * test_audio_interface_sdl.c -- unit tests for Source/audio_interface_sdl.c, the SDL2 audio output backend.
 *
 * White box (the source is #included so the file-static context and mixer callback are reachable) but
 * against the real SDL2 library: SDL_AUDIODRIVER=dummy gives a genuine audio device with its own audio
 * thread and no sound card, so open/pause/callback/close run exactly as in production.
 */
#include "avos_test.h"
#include <stdlib.h>

/*
 * Fault injection for the two allocation-failure paths of sdl_open(): within this translation unit only,
 * malloc/calloc go through wrappers that behave normally unless a test arms them.
 */
static int fail_calloc, fail_malloc;
static void *t_calloc(size_t n, size_t sz) { return fail_calloc ? NULL : calloc(n, sz); }
static void *t_malloc(size_t sz) { return fail_malloc ? NULL : malloc(sz); }
#define calloc t_calloc
#define malloc t_malloc
#include "Source/audio_interface_sdl.c"
#undef calloc
#undef malloc

#include <time.h>
#include <unistd.h>

extern int Debug[];

static void use_dummy_driver(void) { setenv("SDL_AUDIODRIVER", "dummy", 1); }

static struct audio_ctx *open_ctx(void)
{
	use_dummy_driver();
	REQUIRE(sdl_init() == 0);
	struct audio_ctx *c = (struct audio_ctx *)sdl_open(AUDIO_OUTPUT_MODE);
	REQUIRE_NOT_NULL(c);
	return c;
}

static void close_ctx(struct audio_ctx *c)
{
	audio_ctx_t *p = (audio_ctx_t *)c;
	CHECK_EQ_INT(sdl_close(&p), 0);
	sdl_exit();
}

static int wait_until_drained(struct audio_ctx *c, int ms)
{
	for (int i = 0; i < ms; i++) {
		pthread_mutex_lock(&c->lock);
		int n = c->buffer.size;
		pthread_mutex_unlock(&c->lock);
		if (n == 0)
			return 1;
		usleep(1000);
	}
	return 0;
}

/* ---- init / open / close ------------------------------------------------------------------- */

TEST(init_fails_cleanly_for_an_unknown_audio_driver_and_succeeds_for_the_dummy_one)
{
	setenv("SDL_AUDIODRIVER", "no-such-audio-driver", 1);
	CHECK_EQ_INT(sdl_init(), -1);
	use_dummy_driver();
	Debug[DBG_AUDIODEVICE] = 2;
	CHECK_EQ_INT(sdl_init(), 0);
	sdl_exit();
}

TEST(only_output_mode_can_be_opened_and_a_second_open_returns_the_same_context)
{
	use_dummy_driver();
	CHECK_NULL(sdl_open(AUDIO_MIXER_MODE));
	Debug[DBG_AUDIODEVICE] = 1;
	struct audio_ctx *a = (struct audio_ctx *)sdl_open(AUDIO_OUTPUT_MODE);
	REQUIRE_NOT_NULL(a);
	CHECK(sdl_open(AUDIO_OUTPUT_MODE) == (audio_ctx_t *)a);
	CHECK_EQ_INT(a->is_open, 1);
	CHECK_EQ_INT(a->device_id, 0);			/* no device until the parameters are set */
	audio_ctx_t *p = (audio_ctx_t *)a;
	CHECK_EQ_INT(sdl_close(&p), 0);
	CHECK_NULL(p);
	CHECK_NULL(_ctx);
	/* after a close a new context can be opened */
	struct audio_ctx *b = (struct audio_ctx *)sdl_open(AUDIO_OUTPUT_MODE);
	CHECK_NOT_NULL(b);
	p = (audio_ctx_t *)b;
	sdl_close(&p);
}

TEST(open_fails_cleanly_when_memory_runs_out)
{
	use_dummy_driver();
	Debug[DBG_AUDIODEVICE] = 0;
	fail_calloc = 1;
	CHECK_NULL(sdl_open(AUDIO_OUTPUT_MODE));
	fail_calloc = 0;
	fail_malloc = 1;				/* context allocated, sample buffer refused */
	CHECK_NULL(sdl_open(AUDIO_OUTPUT_MODE));
	fail_malloc = 0;
	CHECK_NULL(_ctx);				/* nothing half-registered */
	struct audio_ctx *c = (struct audio_ctx *)sdl_open(AUDIO_OUTPUT_MODE);
	CHECK_NOT_NULL(c);				/* and it recovers */
	audio_ctx_t *p = (audio_ctx_t *)c;
	sdl_close(&p);
}

TEST(close_rejects_null_and_foreign_contexts)
{
	CHECK_EQ_INT(sdl_close(NULL), -1);
	audio_ctx_t *none = NULL;
	CHECK_EQ_INT(sdl_close(&none), -1);
	struct audio_ctx *c = open_ctx();
	struct audio_ctx other;
	audio_ctx_t *foreign = (audio_ctx_t *)&other;
	CHECK_EQ_INT(sdl_close(&foreign), -1);		/* not the open context */
	CHECK(foreign == (audio_ctx_t *)&other);	/* and the caller's handle is left alone */
	close_ctx(c);
}

TEST(close_also_releases_an_open_device)
{
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_set_output_params((audio_ctx_t *)c, 44100, 2, 2, 16, WAVE_FORMAT_PCM), 0);
	CHECK(c->device_id != 0);
	close_ctx(c);
}

/* ---- format and parameter handling ------------------------------------------------------------- */

TEST(every_wave_format_maps_to_16_bit_little_endian_pcm)
{
	static const int fmts[] = { WAVE_FORMAT_PCM, WAVE_FORMAT_UNKNOWN, WAVE_FORMAT_MULAW, WAVE_FORMAT_ALAW,
		WAVE_FORMAT_EAC3, WAVE_FORMAT_E_AC3_JOC, WAVE_FORMAT_AC3, WAVE_FORMAT_AAC, WAVE_FORMAT_MPEGLAYER3,
		WAVE_FORMAT_MPEG, WAVE_FORMAT_IMA, 0x1234 /* unknown */ };
	for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
		SDL_AudioFormat f = 0;
		CHECK_EQ_INT(_convert_to_sdl_format(fmts[i], &f), 0);
		CHECK_EQ_INT(f, AUDIO_S16LSB);
	}
}

TEST(sample_width_follows_the_sdl_format)
{
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_U8), 1);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_S16LSB), 2);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_S16MSB), 2);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_S32LSB), 4);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_S32MSB), 4);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_F32LSB), 4);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_F32MSB), 4);
	CHECK_EQ_INT(_get_bytes_per_sample(AUDIO_S8), 2);	/* anything else: assumed 16 bit */
}

TEST(set_output_params_needs_an_open_context)
{
	CHECK_EQ_INT(sdl_set_output_params(NULL, 48000, 2, 2, 16, WAVE_FORMAT_PCM), -1);
	struct audio_ctx closed = { 0 };
	CHECK_EQ_INT(sdl_set_output_params((audio_ctx_t *)&closed, 48000, 2, 2, 16, WAVE_FORMAT_PCM), -1);
}

TEST(set_output_params_opens_a_paused_device_and_records_the_stream_shape)
{
	Debug[DBG_AUDIODEVICE] = 2;
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM), 0);
	CHECK(c->device_id != 0);
	CHECK_EQ_UINT(c->rate, 48000);
	CHECK_EQ_UINT(c->channels, 2);
	CHECK_EQ_INT(c->byte_per_sample, 2);
	CHECK_EQ_INT(c->spec.freq, 48000);
	CHECK_EQ_INT(c->spec.channels, 2);
	CHECK_EQ_INT(SDL_GetAudioDeviceStatus(c->device_id), SDL_AUDIO_PAUSED);
	close_ctx(c);
}

TEST(reconfiguring_while_stopped_replaces_the_device_and_stays_paused)
{
	struct audio_ctx *c = open_ctx();
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	SDL_AudioDeviceID first = c->device_id;
	CHECK_EQ_INT(sdl_set_output_params((audio_ctx_t *)c, 44100, 1, 1, 16, WAVE_FORMAT_PCM), 0);
	CHECK(c->device_id != 0);
	(void)first;					/* ids may be reused: observable effect is the new shape */
	CHECK_EQ_UINT(c->rate, 44100);
	CHECK_EQ_UINT(c->channels, 1);
	CHECK_EQ_INT(SDL_GetAudioDeviceStatus(c->device_id), SDL_AUDIO_PAUSED);
	close_ctx(c);
}

TEST(reconfiguring_while_playing_resumes_the_new_device_by_itself)
{
	struct audio_ctx *c = open_ctx();
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	CHECK_EQ_INT(sdl_start((audio_ctx_t *)c), 0);
	CHECK_EQ_INT(sdl_set_output_params((audio_ctx_t *)c, 22050, 2, 2, 16, WAVE_FORMAT_PCM), 0);
	CHECK_EQ_INT(SDL_GetAudioDeviceStatus(c->device_id), SDL_AUDIO_PLAYING);
	close_ctx(c);
}

TEST(set_output_params_fails_when_sdl_rejects_the_stream)
{
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_set_output_params((audio_ctx_t *)c, 48000, 9, 9, 16, WAVE_FORMAT_PCM), -1);
	CHECK_EQ_INT(c->device_id, 0);
	close_ctx(c);
}

/* ---- start / stop -------------------------------------------------------------------------------- */

TEST(start_and_stop_need_a_device_and_toggle_the_sdl_pause_state)
{
	CHECK_EQ_INT(sdl_start(NULL), -1);
	CHECK_EQ_INT(sdl_stop(NULL), -1);
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_start((audio_ctx_t *)c), -1);	/* no device yet */
	CHECK_EQ_INT(sdl_stop((audio_ctx_t *)c), -1);
	Debug[DBG_AUDIODEVICE] = 1;
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	CHECK_EQ_INT(sdl_start((audio_ctx_t *)c), 0);
	CHECK_EQ_INT(c->is_playing, 1);
	CHECK_EQ_INT(SDL_GetAudioDeviceStatus(c->device_id), SDL_AUDIO_PLAYING);
	CHECK_EQ_INT(sdl_stop((audio_ctx_t *)c), 0);
	CHECK_EQ_INT(c->is_playing, 0);
	CHECK_EQ_INT(SDL_GetAudioDeviceStatus(c->device_id), SDL_AUDIO_PAUSED);
	close_ctx(c);
}

/* ---- writing and the ring buffer ---------------------------------------------------------------------- */

TEST(write_validates_its_arguments)
{
	unsigned char b[16] = { 0 };
	CHECK_EQ_INT(sdl_write(NULL, b, 16), 0);
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, b, 16), 0);		/* no device */
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, NULL, 16), 0);
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, b, 0), 0);
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, b, -4), 0);
	CHECK_EQ_INT(c->buffer.size, 0);
	close_ctx(c);
}

TEST(write_queues_bytes_in_order_until_the_ring_buffer_is_full)
{
	struct audio_ctx *c = open_ctx();
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);	/* stays paused: nothing drains */
	const int total = SDL_AUDIO_BUFFER_SIZE * SDL_AUDIO_BUFFER_COUNT;
	unsigned char blk[1000];
	for (int i = 0; i < 1000; i++) blk[i] = (unsigned char)i;
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, blk, 1000), 1000);
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, blk, 500), 500);
	CHECK_EQ_INT(c->buffer.size, 1500);
	CHECK_EQ_MEM(c->buffer.data, blk, 1000);
	CHECK_EQ_MEM(c->buffer.data + 1000, blk, 500);

	unsigned char *big = calloc(1, total);
	REQUIRE_NOT_NULL(big);
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, big, total), total - 1500);	/* only what fits */
	CHECK_EQ_INT(c->buffer.size, total);
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, blk, 10), 0);			/* full */
	free(big);
	close_ctx(c);
}

TEST(can_write_compares_the_request_with_the_free_space)
{
	CHECK_EQ_INT(sdl_can_write(NULL, 1), 0);
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_can_write((audio_ctx_t *)c, 1), 0);		/* no device */
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	const int total = SDL_AUDIO_BUFFER_SIZE * SDL_AUDIO_BUFFER_COUNT;
	CHECK_EQ_INT(sdl_can_write((audio_ctx_t *)c, total), 1);
	CHECK_EQ_INT(sdl_can_write((audio_ctx_t *)c, total + 1), 0);
	c->buffer.size = total - 100;
	CHECK_EQ_INT(sdl_can_write((audio_ctx_t *)c, 100), 1);
	CHECK_EQ_INT(sdl_can_write((audio_ctx_t *)c, 101), 0);
	c->buffer.size = 0;
	close_ctx(c);
}

TEST(flush_drops_everything_that_was_queued)
{
	sdl_flush_output(NULL);					/* harmless */
	Debug[DBG_AUDIODEVICE] = 1;
	struct audio_ctx *c = open_ctx();
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	unsigned char b[2000] = { 0 };
	sdl_write((audio_ctx_t *)c, b, sizeof b);
	c->buffer.pos = 7;
	sdl_flush_output((audio_ctx_t *)c);
	CHECK_EQ_INT(c->buffer.size, 0);
	CHECK_EQ_INT(c->buffer.pos, 0);
	close_ctx(c);
}

/* ---- the SDL audio callback --------------------------------------------------------------------------- */

TEST(callback_copies_queued_audio_shifts_the_remainder_and_pads_with_silence)
{
	struct audio_ctx *c = open_ctx();			/* no device: we drive the callback ourselves */
	unsigned char in[100];
	for (int i = 0; i < 100; i++) in[i] = (unsigned char)(i + 1);
	memcpy(c->buffer.data, in, 100);
	c->buffer.size = 100;

	unsigned char out[60];
	memset(out, 0xEE, sizeof out);
	sdl_audio_callback(c, out, 60);				/* less than queued: pure copy */
	CHECK_EQ_MEM(out, in, 60);
	CHECK_EQ_INT(c->buffer.size, 40);
	CHECK_EQ_MEM(c->buffer.data, in + 60, 40);		/* remainder moved to the front */

	memset(out, 0xEE, sizeof out);
	sdl_audio_callback(c, out, 60);				/* more than queued: copy + zero fill */
	CHECK_EQ_MEM(out, in + 60, 40);
	for (int i = 40; i < 60; i++) CHECK_EQ_INT(out[i], 0);
	CHECK_EQ_INT(c->buffer.size, 0);

	memset(out, 0xEE, sizeof out);
	sdl_audio_callback(c, out, 60);				/* empty queue: all silence */
	for (int i = 0; i < 60; i++) CHECK_EQ_INT(out[i], 0);

	memset(out, 0xEE, sizeof out);
	unsigned char *saved = c->buffer.data;
	c->buffer.data = NULL;					/* no buffer at all: still silence, no crash */
	sdl_audio_callback(c, out, 60);
	for (int i = 0; i < 60; i++) CHECK_EQ_INT(out[i], 0);
	c->buffer.data = saved;

	/* exact fit leaves nothing to shift */
	memcpy(c->buffer.data, in, 60);
	c->buffer.size = 60;
	sdl_audio_callback(c, out, 60);
	CHECK_EQ_MEM(out, in, 60);
	CHECK_EQ_INT(c->buffer.size, 0);
	close_ctx(c);
}

TEST(a_started_device_really_consumes_the_queued_audio)
{
	struct audio_ctx *c = open_ctx();
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	static unsigned char pcm[19200];			/* 100 ms of silence */
	CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, pcm, sizeof pcm), (int)sizeof pcm);
	usleep(100 * 1000);
	CHECK_EQ_INT(c->buffer.size, (int)sizeof pcm);		/* paused: nothing consumed */
	sdl_start((audio_ctx_t *)c);
	CHECK(wait_until_drained(c, 3000));			/* the SDL audio thread drained it */
	close_ctx(c);
}

/* ---- delay ----------------------------------------------------------------------------------------------- */

TEST(delay_is_zero_until_the_stream_shape_is_known)
{
	CHECK_EQ_INT(sdl_get_delay(NULL), 0);
	struct audio_ctx *c = open_ctx();
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), 0);
	close_ctx(c);
}

TEST(delay_is_queued_time_plus_one_device_buffer)
{
	struct audio_ctx *c = open_ctx();
	sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
	unsigned samples = c->spec.samples;
	int dev_ms = samples * 1000 / c->spec.freq;
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), dev_ms);		/* empty ring: only the device buffer */
	static unsigned char pcm[48000 * 4 / 4];			/* 250 ms of S16 stereo */
	sdl_write((audio_ctx_t *)c, pcm, sizeof pcm);
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), 250 + dev_ms);
	c->spec.samples = 0;						/* a driver reporting no buffer size */
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), 250);
	close_ctx(c);
}

/* ---- trivial and unsupported features -------------------------------------------------------------------- */

TEST(unsupported_features_report_their_fixed_answers)
{
	Debug[DBG_AUDIODEVICE] = 1;
	CHECK_EQ_INT(sdl_preload(NULL), -1);
	CHECK_EQ_INT(sdl_mute(NULL, TRUE), 0);
	CHECK_EQ_INT(sdl_unmute(NULL, TRUE, FALSE), 0);
	CHECK_EQ_INT(sdl_set_output_volume(NULL, 50, 0), 0);
	CHECK_EQ_INT(sdl_set_output_volume_l_r(NULL, 10, 20), 0);
	CHECK_EQ_INT(sdl_get_session_id(NULL), 0);
	CHECK_EQ_INT(sdl_get_passthrough(NULL), 0);
	CHECK_EQ_INT(sdl_set_passthrough(NULL, 0), 0);
	CHECK_EQ_INT(sdl_set_passthrough(NULL, 1), 0);			/* refused, stays PCM, still not an error */
}

TEST(the_backend_table_wires_every_function_and_leaves_pause_to_the_dispatcher)
{
	const audio_interface_impl_t *t = &audio_interface_impl_sdl;
	CHECK_EQ_STR(t->name, "sdl");
	CHECK(t->init == sdl_init && t->exit == sdl_exit && t->open == sdl_open && t->close == sdl_close);
	CHECK(t->start == sdl_start && t->stop == sdl_stop && t->write == sdl_write && t->can_write == sdl_can_write);
	CHECK(t->set_output_params == sdl_set_output_params && t->get_delay == sdl_get_delay);
	CHECK(t->flush_output == sdl_flush_output && t->preload == sdl_preload);
	CHECK(t->set_passthrough == sdl_set_passthrough && t->get_passthrough == sdl_get_passthrough);
	CHECK_NULL(t->pause);
	CHECK_NULL(t->unpause);
	CHECK_NULL(t->get_latency);
}

/* ---- branch completeness: debug logging off/on, partial stream shapes, degenerate callbacks ------------------ */

TEST(behaviour_is_identical_with_debug_logging_off_and_on)
{
	for (int level = 0; level <= 1; level++) {
		Debug[DBG_AUDIODEVICE] = level;
		struct audio_ctx *c = open_ctx();
		CHECK(sdl_open(AUDIO_OUTPUT_MODE) == (audio_ctx_t *)c);	/* already open */
		sdl_set_output_params((audio_ctx_t *)c, 48000, 2, 2, 16, WAVE_FORMAT_PCM);
		CHECK_EQ_INT(sdl_start((audio_ctx_t *)c), 0);
		CHECK_EQ_INT(sdl_stop((audio_ctx_t *)c), 0);
		unsigned char b[64] = { 0 };
		CHECK_EQ_INT(sdl_write((audio_ctx_t *)c, b, 64), 64);
		sdl_flush_output((audio_ctx_t *)c);
		CHECK_EQ_INT(c->buffer.size, 0);
		CHECK_EQ_INT(sdl_mute((audio_ctx_t *)c, FALSE), 0);
		CHECK_EQ_INT(sdl_unmute((audio_ctx_t *)c, FALSE, FALSE), 0);
		CHECK_EQ_INT(sdl_set_output_volume((audio_ctx_t *)c, 1, 1), 0);
		CHECK_EQ_INT(sdl_set_output_volume_l_r((audio_ctx_t *)c, 1, 1), 0);
		close_ctx(c);
	}
}

TEST(delay_needs_both_a_rate_and_a_channel_count)
{
	struct audio_ctx *c = open_ctx();
	c->rate = 48000; c->channels = 0; c->byte_per_sample = 2;
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), 0);
	c->rate = 0; c->channels = 2;
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), 0);
	c->rate = 48000;
	c->spec.samples = 2048; c->spec.freq = 0;		/* device buffer size known but not its rate */
	CHECK_EQ_INT(sdl_get_delay((audio_ctx_t *)c), 0);
	close_ctx(c);
}

TEST(callback_with_nothing_to_deliver_leaves_the_queue_alone)
{
	struct audio_ctx *c = open_ctx();
	unsigned char in[10] = { 1, 2, 3 };
	memcpy(c->buffer.data, in, 10);
	c->buffer.size = 10;
	unsigned char out[4] = { 9, 9, 9, 9 };
	sdl_audio_callback(c, out, 0);				/* zero-length request */
	CHECK_EQ_INT(c->buffer.size, 10);
	CHECK_EQ_INT(out[0], 9);
	close_ctx(c);
}

TEST(close_copes_with_a_context_whose_buffer_is_already_gone)
{
	struct audio_ctx *c = open_ctx();
	free(c->buffer.data);
	c->buffer.data = NULL;
	audio_ctx_t *p = (audio_ctx_t *)c;
	CHECK_EQ_INT(sdl_close(&p), 0);
	sdl_exit();
}
