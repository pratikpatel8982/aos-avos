/*
 * test_audio_interface.c -- unit tests for Source/audio_interface.c (the audio output dispatcher) and
 * Source/audio_interface_null.c (the silent backend).
 *
 * White box: the dispatcher is compiled into the test so that its private `impl` pointer can be set.
 * Backends used:
 *   - the real null backend (audio_interface_impl_null) for everything it implements and, because it
 *     leaves most optional slots empty, for every "slot missing -> default answer" branch;
 *   - the real SDL backend through the real audio_interface_init(), run against SDL's own "dummy" audio
 *     driver (no sound card needed) for the host's normal selection path;
 *   - a recording table of the backend contract (audio_interface_impl_t) whose functions return distinctive
 *     values, to prove that every optional slot is forwarded with the right arguments when present.
 *     The table is the dispatcher's input, not production code.
 */
#include "avos_test.h"
#include "Source/audio_interface.c"

#include <stdlib.h>
#include <string.h>

extern const audio_interface_impl_t audio_interface_impl_null;
audio_ctx_t *null_open(int mode);

/* ---- a recording backend ----------------------------------------------------------------- */

static struct {
	int calls;
	const char *last;
	audio_ctx_t *ctx;
	int a, b, c, d, e;
	float f;
	uint64_t *frames;
} rec;

#define REC(name) do { rec.calls++; rec.last = name; rec.ctx = ctx; } while (0)

static int  r_init(void)                    { rec.calls++; rec.last = "init"; return 0; }
static void r_exit(void)                    { rec.calls++; rec.last = "exit"; }
static audio_ctx_t *r_open(int m)           { rec.calls++; rec.last = "open"; rec.a = m; return (audio_ctx_t *)0x1000; }
static int  r_close(audio_ctx_t **p)        { rec.calls++; rec.last = "close"; rec.ctx = *p; return 11; }
static int  r_start(audio_ctx_t *ctx)       { REC("start"); return 12; }
static int  r_stop(audio_ctx_t *ctx)        { REC("stop"); return 13; }
static int  r_pause(audio_ctx_t *ctx)       { REC("pause"); return 14; }
static int  r_unpause(audio_ctx_t *ctx)     { REC("unpause"); return 15; }
static int  r_can_write(audio_ctx_t *ctx, int n) { REC("can_write"); rec.a = n; return 16; }
static int  r_write(audio_ctx_t *ctx, unsigned char *d, int n) { REC("write"); rec.a = n; rec.b = d ? d[0] : -1; return 17; }
static int  r_set_params(audio_ctx_t *ctx, int fq, int ch, int cc, int bits, int fmt)
{ REC("set_output_params"); rec.a = fq; rec.b = ch; rec.c = cc; rec.d = bits; rec.e = fmt; return 18; }
static int  r_get_delay(audio_ctx_t *ctx)   { REC("get_delay"); return 19; }
static int  r_get_latency(audio_ctx_t *ctx) { REC("get_latency"); return 20; }
static int  r_get_pipeline_latency(audio_ctx_t *ctx) { REC("get_pipeline_latency"); return 21; }
static int  r_get_fixed_latency(audio_ctx_t *ctx) { REC("get_fixed_latency"); return 22; }
static void r_flush(audio_ctx_t *ctx)       { REC("flush_output"); }
static int  r_preload(audio_ctx_t *ctx)     { REC("preload"); return 23; }
static int  r_mute(audio_ctx_t *ctx, BOOL fade) { REC("mute"); rec.a = fade; return 24; }
static int  r_unmute(audio_ctx_t *ctx, BOOL fade, BOOL thr) { REC("unmute"); rec.a = fade; rec.b = thr; return 25; }
static int  r_set_vol(audio_ctx_t *ctx, int v, int bal) { REC("set_output_volume"); rec.a = v; rec.b = bal; return 26; }
static int  r_set_vol_lr(audio_ctx_t *ctx, int l, int r) { REC("set_output_volume_l_r"); rec.a = l; rec.b = r; return 27; }
static int  r_session(audio_ctx_t *ctx)     { REC("get_session_id"); return 28; }
static int  r_get_pass(audio_ctx_t *ctx)    { REC("get_passthrough"); return 29; }
static int  r_set_pass(audio_ctx_t *ctx, int p) { REC("set_passthrough"); rec.a = p; return 30; }
static int  r_speed(audio_ctx_t *ctx, float s) { REC("change_audio_speed"); rec.f = s; return 31; }
static int  r_delay_valid(audio_ctx_t *ctx) { REC("delay_valid"); return 0; }
static const char *r_delay_src(audio_ctx_t *ctx) { REC("delay_source"); return "recording"; }
static int  r_streak(audio_ctx_t *ctx)      { REC("delay_valid_streak"); return 32; }
static int  r_hold(audio_ctx_t *ctx)        { REC("is_startup_hold_active"); return 33; }
static int  r_pt_adv(audio_ctx_t *ctx)      { REC("passthrough_playhead_advanced"); return 0; }
static void r_inval(audio_ctx_t *ctx)       { REC("invalidate_delay_cache"); }
static void r_logical(audio_ctx_t *ctx, int s, int b) { REC("add_logical_samples"); rec.a = s; rec.b = b; }
static int  r_delta(audio_ctx_t *ctx)       { REC("get_and_clear_latency_delta"); return 34; }
static int  r_presented(audio_ctx_t *ctx, uint64_t *fr, int *rate, int *src, int *age, int fresh)
{ REC("get_presented_frames"); *fr = 777; *rate = 48000; *src = 1; *age = 5; rec.a = fresh; return 1; }
static int  r_written(audio_ctx_t *ctx, uint64_t *fr, int *rate) { REC("get_written_frames"); *fr = 888; *rate = 44100; return 1; }
static int  r_snapshot(audio_ctx_t *ctx, AUDIO_PRESENTATION_SNAPSHOT *s) { REC("get_presentation_snapshot"); s->generation = 99; return 1; }

static audio_interface_impl_t full_impl = {
	.name = "recording", .init = r_init, .exit = r_exit, .open = r_open, .close = r_close, .start = r_start,
	.stop = r_stop, .pause = r_pause, .unpause = r_unpause, .can_write = r_can_write, .write = r_write,
	.set_output_params = r_set_params, .get_delay = r_get_delay, .get_latency = r_get_latency,
	.get_pipeline_latency = r_get_pipeline_latency, .get_fixed_latency = r_get_fixed_latency,
	.flush_output = r_flush, .preload = r_preload, .mute = r_mute, .unmute = r_unmute,
	.set_output_volume = r_set_vol, .set_output_volume_l_r = r_set_vol_lr, .get_session_id = r_session,
	.get_passthrough = r_get_pass, .set_passthrough = r_set_pass, .change_audio_speed = r_speed,
	.delay_valid = r_delay_valid, .delay_source = r_delay_src, .delay_valid_streak = r_streak,
	.is_startup_hold_active = r_hold, .passthrough_playhead_advanced = r_pt_adv,
	.invalidate_delay_cache = r_inval, .add_logical_samples = r_logical,
	.get_and_clear_latency_delta = r_delta, .get_presented_frames = r_presented,
	.get_written_frames = r_written, .get_presentation_snapshot = r_snapshot,
	.pause_preserves_output = 1,
};

static audio_ctx_t *const CTX = (audio_ctx_t *)0x5150;

/* ---- the null backend ----------------------------------------------------------------------- */

TEST(null_backend_accepts_every_byte_and_reports_no_delay)
{
	const audio_interface_impl_t *n = &audio_interface_impl_null;
	CHECK_EQ_STR(n->name, "null");
	CHECK_EQ_INT(n->init(), 0);
	audio_ctx_t *c = n->open(AUDIO_OUTPUT_MODE);
	CHECK_NOT_NULL(c);
	CHECK_EQ_INT(n->set_output_params(c, 48000, 2, 2, 16, 1), 0);
	CHECK_EQ_INT(n->start(c), 0);
	CHECK_EQ_INT(n->can_write(c, 1 << 20), 1);
	unsigned char pcm[1000] = { 0 };
	CHECK_EQ_INT(n->write(c, pcm, sizeof pcm), 1000);
	CHECK_EQ_INT(n->write(c, pcm, 0), 0);
	CHECK_EQ_INT(n->get_delay(c), 0);
	CHECK_EQ_INT(n->preload(c), 0);
	n->flush_output(c);
	CHECK_EQ_INT(n->stop(c), 0);
	CHECK_EQ_INT(n->close(&c), 0);
	CHECK_NULL(c);					/* the handle is cleared on close */
	n->exit();
}

TEST(null_backend_leaves_the_optional_features_unimplemented)
{
	const audio_interface_impl_t *n = &audio_interface_impl_null;
	CHECK_NULL(n->pause);
	CHECK_NULL(n->unpause);
	CHECK_NULL(n->get_latency);
	CHECK_NULL(n->mute);
	CHECK_NULL(n->set_output_volume);
	CHECK_NULL(n->change_audio_speed);
	CHECK_EQ_INT(n->pause_preserves_output, 0);
}

TEST(null_backend_hands_out_distinct_contexts)
{
	audio_ctx_t *a = null_open(0), *b = null_open(0);
	CHECK_NOT_NULL(a);
	CHECK_NOT_NULL(b);
	CHECK(a != b);
	audio_interface_impl_null.close(&a);
	audio_interface_impl_null.close(&b);
}

/* ---- backend selection on a host ----------------------------------------------------------------- */

TEST(init_selects_the_real_sdl_backend_when_there_is_no_oss_device_and_exit_releases_it)
{
	setenv("SDL_AUDIODRIVER", "dummy", 1);
	REQUIRE(access("/dev/dsp", W_OK) != 0 && access("/dev/snd/dsp", W_OK) != 0);
	CHECK_NULL(audio_interface_open(AUDIO_OUTPUT_MODE));	/* nothing selected yet */
	CHECK_EQ_INT(audio_interface_init(), 0);
	CHECK(impl == &audio_interface_impl_sdl);
	CHECK_EQ_STR(impl->name, "sdl");
	CHECK_EQ_INT(audio_interface_exit(), 0);
	CHECK_NULL(impl);
	CHECK_EQ_INT(audio_interface_exit(), 0);		/* exiting twice is harmless */
}

TEST(init_reports_failure_when_neither_oss_nor_sdl_can_start)
{
	setenv("SDL_AUDIODRIVER", "no-such-audio-driver", 1);
	CHECK_EQ_INT(audio_interface_init(), -1);
	CHECK_NULL(impl);
	CHECK_NULL(audio_interface_open(AUDIO_OUTPUT_MODE));
}

TEST(the_aif_debug_command_re_runs_the_selection)
{
	setenv("SDL_AUDIODRIVER", "dummy", 1);
	debug_do_cmd((const unsigned char *)"aif 3");
	CHECK_EQ_INT(audio_interface_force, 3);
	CHECK(impl == &audio_interface_impl_sdl);
	debug_do_cmd((const unsigned char *)"aif");		/* no argument: nothing changes */
	CHECK_EQ_INT(audio_interface_force, 3);
	audio_interface_exit();
}

/* ---- every call forwarded to a backend that implements it ----------------------------------------- */

TEST(core_calls_are_forwarded_with_their_arguments_and_results)
{
	impl = &full_impl;
	memset(&rec, 0, sizeof rec);
	CHECK(audio_interface_open(AUDIO_MIXER_MODE) == (audio_ctx_t *)0x1000);
	CHECK_EQ_INT(rec.a, AUDIO_MIXER_MODE);
	audio_ctx_t *c = CTX;
	CHECK_EQ_INT(audio_interface_close(&c), 11);
	CHECK(rec.ctx == CTX);
	CHECK_EQ_INT(audio_interface_start(CTX), 12);
	CHECK_EQ_INT(audio_interface_stop(CTX), 13);
	CHECK_EQ_INT(audio_interface_pause(CTX), 14);
	CHECK_EQ_STR(rec.last, "pause");
	CHECK_EQ_INT(audio_interface_unpause(CTX), 15);
	CHECK_EQ_STR(rec.last, "unpause");
	CHECK_EQ_INT(audio_interface_can_write(CTX, 4096), 16);
	CHECK_EQ_INT(rec.a, 4096);
	unsigned char d[2] = { 0x7E, 0 };
	CHECK_EQ_INT(audio_interface_write(CTX, d, 2), 17);
	CHECK_EQ_INT(rec.a, 2);
	CHECK_EQ_INT(rec.b, 0x7E);
	CHECK_EQ_INT(audio_interface_set_output_params(CTX, 44100, 6, 8, 24, 3), 18);
	CHECK(rec.a == 44100 && rec.b == 6 && rec.c == 8 && rec.d == 24 && rec.e == 3);
	CHECK_EQ_INT(audio_interface_get_delay(CTX), 19);
	audio_interface_flush_output(CTX);
	CHECK_EQ_STR(rec.last, "flush_output");
	CHECK_EQ_INT(audio_interface_preload(CTX), 23);
	impl = NULL;
}

TEST(optional_calls_are_forwarded_when_the_backend_provides_them)
{
	impl = &full_impl;
	memset(&rec, 0, sizeof rec);
	CHECK_EQ_INT(audio_interface_get_latency(CTX), 20);
	CHECK_EQ_INT(audio_interface_get_pipeline_latency(CTX), 21);
	CHECK_EQ_INT(audio_interface_get_fixed_latency(CTX), 22);
	CHECK_EQ_INT(audio_interface_is_delay_valid(CTX), 0);
	CHECK_EQ_STR(audio_interface_get_delay_source(CTX), "recording");
	CHECK_EQ_INT(audio_interface_get_delay_valid_streak(CTX), 32);
	CHECK_EQ_INT(audio_interface_is_startup_hold_active(CTX), 33);
	CHECK_EQ_INT(audio_interface_passthrough_playhead_advanced(CTX), 0);
	audio_interface_invalidate_delay_cache(CTX);
	CHECK_EQ_STR(rec.last, "invalidate_delay_cache");
	audio_interface_add_logical_samples(CTX, 1536, 6144);
	CHECK(rec.a == 1536 && rec.b == 6144);
	CHECK_EQ_INT(audio_interface_get_and_clear_latency_delta(CTX), 34);
	CHECK_EQ_INT(audio_interface_mute(CTX, TRUE), 24);
	CHECK_EQ_INT(rec.a, TRUE);
	CHECK_EQ_INT(audio_interface_unmute(CTX, TRUE, FALSE), 25);
	CHECK(rec.a == TRUE && rec.b == FALSE);
	CHECK_EQ_INT(audio_interface_set_output_volume(CTX, 70, -3), 26);
	CHECK(rec.a == 70 && rec.b == -3);
	CHECK_EQ_INT(audio_interface_set_output_volume_l_r(CTX, 10, 90), 27);
	CHECK(rec.a == 10 && rec.b == 90);
	CHECK_EQ_INT(audio_interface_get_session_id(CTX), 28);
	CHECK_EQ_INT(audio_interface_get_passthrough(CTX), 29);
	CHECK_EQ_INT(audio_interface_set_passthrough(CTX, 2), 30);
	CHECK_EQ_INT(rec.a, 2);
	CHECK_EQ_INT(audio_interface_change_audio_speed(CTX, 1.5f), 31);
	CHECK(rec.f == 1.5f);
	uint64_t frames = 0; int rate = 0, src = 0, age = 0;
	CHECK_EQ_INT(audio_interface_get_presented_frames(CTX, &frames, &rate, &src, &age, 2), 1);
	CHECK(frames == 777 && rate == 48000 && src == 1 && age == 5);
	CHECK_EQ_INT(rec.a, 2);
	CHECK_EQ_INT(audio_interface_get_written_frames(CTX, &frames, &rate), 1);
	CHECK(frames == 888 && rate == 44100);
	AUDIO_PRESENTATION_SNAPSHOT snap;
	memset(&snap, 0, sizeof snap);
	CHECK_EQ_INT(audio_interface_get_presentation_snapshot(CTX, &snap), 1);
	CHECK(snap.generation == 99);
	impl = NULL;
}

TEST(pause_preservation_needs_a_context_a_backend_a_pause_slot_and_the_opt_in_flag)
{
	impl = &full_impl;
	CHECK_EQ_INT(audio_interface_pause_preserves_output(CTX), 1);
	CHECK_EQ_INT(audio_interface_pause_preserves_output(NULL), 0);
	full_impl.pause_preserves_output = 0;
	CHECK_EQ_INT(audio_interface_pause_preserves_output(CTX), 0);
	full_impl.pause_preserves_output = 1;
	impl = NULL;
	CHECK_EQ_INT(audio_interface_pause_preserves_output(CTX), 0);
	impl = &audio_interface_impl_null;		/* no pause slot */
	CHECK_EQ_INT(audio_interface_pause_preserves_output(CTX), 0);
	impl = NULL;
}

/* ---- a backend that implements only the mandatory calls -------------------------------------------- */

TEST(missing_optional_slots_give_conservative_defaults)
{
	impl = &audio_interface_impl_null;
	CHECK_EQ_INT(audio_interface_get_latency(CTX), -1);
	CHECK_EQ_INT(audio_interface_get_pipeline_latency(CTX), -1);	/* falls back to get_latency */
	CHECK_EQ_INT(audio_interface_get_fixed_latency(CTX), 0);
	CHECK_EQ_INT(audio_interface_is_delay_valid(CTX), 1);
	CHECK_EQ_STR(audio_interface_get_delay_source(CTX), "unsupported");
	CHECK_EQ_INT(audio_interface_get_delay_valid_streak(CTX), 0);
	CHECK_EQ_INT(audio_interface_is_startup_hold_active(CTX), 0);
	CHECK_EQ_INT(audio_interface_passthrough_playhead_advanced(CTX), 1);
	audio_interface_invalidate_delay_cache(CTX);
	audio_interface_add_logical_samples(CTX, 1, 1);
	CHECK_EQ_INT(audio_interface_get_and_clear_latency_delta(CTX), 0);
	uint64_t frames = 5; int rate = 6, src = 7, age = 8;
	CHECK_EQ_INT(audio_interface_get_presented_frames(CTX, &frames, &rate, &src, &age, 0), 0);
	CHECK_EQ_INT(audio_interface_get_written_frames(CTX, &frames, &rate), 0);
	CHECK(frames == 5 && rate == 6);				/* outputs untouched */
	AUDIO_PRESENTATION_SNAPSHOT snap;
	CHECK_EQ_INT(audio_interface_get_presentation_snapshot(CTX, &snap), 0);
	CHECK_EQ_INT(audio_interface_mute(CTX, FALSE), -1);
	CHECK_EQ_INT(audio_interface_unmute(CTX, FALSE, FALSE), -1);
	CHECK_EQ_INT(audio_interface_set_output_volume(CTX, 1, 0), -1);
	CHECK_EQ_INT(audio_interface_set_output_volume_l_r(CTX, 1, 1), -1);
	CHECK_EQ_INT(audio_interface_get_session_id(CTX), -1);
	CHECK_EQ_INT(audio_interface_set_passthrough(CTX, 1), -1);
	CHECK_EQ_INT(audio_interface_get_passthrough(CTX), 0);
	CHECK_EQ_INT(audio_interface_change_audio_speed(CTX, 2.0f), -1);
	impl = NULL;
}

TEST(pause_and_unpause_fall_back_to_stop_and_start)
{
	/* the null backend has no pause/unpause: the dispatcher maps them to stop/start (both return 0) */
	impl = &audio_interface_impl_null;
	CHECK_EQ_INT(audio_interface_pause(CTX), 0);
	CHECK_EQ_INT(audio_interface_unpause(CTX), 0);

	audio_interface_impl_t no_pause = full_impl;
	no_pause.pause = NULL;
	no_pause.unpause = NULL;
	impl = &no_pause;
	memset(&rec, 0, sizeof rec);
	CHECK_EQ_INT(audio_interface_pause(CTX), 13);			/* stop's result */
	CHECK_EQ_STR(rec.last, "stop");
	CHECK_EQ_INT(audio_interface_unpause(CTX), 12);			/* start's result */
	CHECK_EQ_STR(rec.last, "start");

	no_pause.stop = NULL;
	no_pause.start = NULL;
	CHECK_EQ_INT(audio_interface_pause(CTX), -1);
	CHECK_EQ_INT(audio_interface_unpause(CTX), -1);
	CHECK_EQ_INT(audio_interface_preload(CTX), 23);
	no_pause.preload = NULL;
	CHECK_EQ_INT(audio_interface_preload(CTX), -1);
	impl = NULL;
}

TEST(a_null_backend_pointer_makes_the_guarded_queries_answer_defaults_instead_of_crashing)
{
	impl = NULL;
	CHECK_EQ_INT(audio_interface_get_latency(CTX), -1);
	CHECK_EQ_INT(audio_interface_get_pipeline_latency(CTX), -1);
	CHECK_EQ_INT(audio_interface_get_fixed_latency(CTX), 0);
	CHECK_EQ_INT(audio_interface_is_delay_valid(CTX), 1);
	CHECK_EQ_STR(audio_interface_get_delay_source(CTX), "unsupported");
	CHECK_EQ_INT(audio_interface_get_delay_valid_streak(CTX), 0);
	CHECK_EQ_INT(audio_interface_is_startup_hold_active(CTX), 0);
	CHECK_EQ_INT(audio_interface_passthrough_playhead_advanced(CTX), 1);
	CHECK_EQ_INT(audio_interface_get_and_clear_latency_delta(CTX), 0);
	CHECK_EQ_INT(audio_interface_get_presented_frames(CTX, NULL, NULL, NULL, NULL, 0), 0);
	CHECK_EQ_INT(audio_interface_get_written_frames(CTX, NULL, NULL), 0);
	CHECK_EQ_INT(audio_interface_get_presentation_snapshot(CTX, NULL), 0);
	CHECK_EQ_INT(audio_interface_pause_preserves_output(CTX), 0);
	audio_interface_invalidate_delay_cache(CTX);
	audio_interface_add_logical_samples(CTX, 1, 1);
	impl = &full_impl;
	CHECK_EQ_INT(audio_interface_get_presentation_snapshot(CTX, NULL), 0);	/* no snapshot buffer: refused */
	impl = NULL;
}

/* ---- end to end through the real null backend ------------------------------------------------------ */

TEST(a_full_session_through_the_dispatcher_and_the_null_backend)
{
	impl = &audio_interface_impl_null;
	audio_ctx_t *c = audio_interface_open(AUDIO_OUTPUT_MODE);
	REQUIRE_NOT_NULL(c);
	CHECK_EQ_INT(audio_interface_set_output_params(c, 48000, 2, 2, 16, 1), 0);
	CHECK_EQ_INT(audio_interface_start(c), 0);
	unsigned char pcm[4096] = { 0 };
	CHECK_EQ_INT(audio_interface_can_write(c, sizeof pcm), 1);
	CHECK_EQ_INT(audio_interface_write(c, pcm, sizeof pcm), (int)sizeof pcm);
	CHECK_EQ_INT(audio_interface_get_delay(c), 0);
	audio_interface_flush_output(c);
	CHECK_EQ_INT(audio_interface_stop(c), 0);
	CHECK_EQ_INT(audio_interface_close(&c), 0);
	CHECK_NULL(c);
	impl = NULL;
}

/* ---- playback speed and tempo bookkeeping ---------------------------------------------------------- */

TEST(audio_speed_remembers_the_previous_value)
{
	CHECK(audio_interface_get_audio_speed() == 1.0f);
	CHECK(audio_interface_get_previous_audio_speed() == 1.0f);
	audio_interface_set_audio_speed(1.5f);
	CHECK(audio_interface_get_audio_speed() == 1.5f);
	CHECK(audio_interface_get_previous_audio_speed() == 1.0f);
	audio_interface_set_audio_speed(0.75f);
	CHECK(audio_interface_get_audio_speed() == 0.75f);
	CHECK(audio_interface_get_previous_audio_speed() == 1.5f);
	audio_interface_set_audio_speed(0.75f);			/* same value again: previous becomes equal */
	CHECK(audio_interface_get_previous_audio_speed() == 0.75f);
}

TEST(speed_and_atempo_switches_are_plain_flags)
{
	CHECK_EQ_INT(audio_interface_is_audio_speed_enabled(), 0);
	audio_interface_enable_audio_speed(1);
	CHECK_EQ_INT(audio_interface_is_audio_speed_enabled(), 1);
	audio_interface_enable_audio_speed(0);
	CHECK_EQ_INT(audio_interface_is_audio_speed_enabled(), 0);
	CHECK_EQ_INT(audio_interface_is_using_atempo(), 0);
	audio_interface_set_using_atempo(1);
	CHECK_EQ_INT(audio_interface_is_using_atempo(), 1);
	audio_interface_set_using_atempo(0);
	CHECK_EQ_INT(audio_interface_is_using_atempo(), 0);
}

/*
 * UNREACHABLE on this host: Source/audio_interface.c:115-116 (OSS selected because audio_interface_impl_oss.init()
 * succeeded) needs a writable /dev/dsp or /dev/snd/dsp; the test above asserts the device is absent.
 *
 * UNREACHABLE on host: the CONFIG_ANDROID selection block (forced backends 0-3, libavos-forced choice,
 * API-level choice and the fallback loop over audiotrack/opensles) and the __APPLE__ branch: they are
 * compiled out of a Linux host build. They are covered by test_audio_interface_android.c, which compiles
 * the same file with CONFIG_ANDROID.
 */
