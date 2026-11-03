/*
 * test_ac_av.c -- unit tests for Source/ac_av.c (the "currently playing AV" registry and dispatcher).
 *
 * ac_av.c keeps one record (state, type, fs, ctx, ID3 tag, callback table) and forwards pause /
 * seek / stop / time queries to whoever registered it. The callback table is the module's input
 * contract, so the tests register a real AV_CALLBACKS table whose functions record their calls and
 * return distinctive values; everything else is the real production code, compiled in white-box
 * (with DEBUG_MSG so that its avs/avp/avt debug commands run through the real debug registry).
 */
#include "avos_test.h"
#include "Source/ac_av.c"

#include <string.h>

/* ---- a registered client ------------------------------------------------------------------ */

static struct {
	int stop, pause, un_pause, is_paused, time, path, seek;
	void *last_ctx;
	int last_stop_arg, last_unpause_arg, last_seek_time, last_seek_pos;
	int paused;				/* what is_paused reports and pause() sets */
	BROWSE_ENTRY *last_br;
} rec;

static int my_ctx;
static const char *CURRENT_PATH = "/media/song.mp3";

static void cb_stop(void *ctx, int allow_resume)       { rec.stop++; rec.last_ctx = ctx; rec.last_stop_arg = allow_resume; }
static int  cb_pause(void *ctx)                        { rec.pause++; rec.last_ctx = ctx; rec.paused = 1; return 111; }
static void cb_un_pause(void *ctx, int was_paused)     { rec.un_pause++; rec.last_ctx = ctx; rec.last_unpause_arg = was_paused; rec.paused = 0; }
static int  cb_is_paused(void *ctx)                    { rec.is_paused++; rec.last_ctx = ctx; return rec.paused; }
static int  cb_time(void *ctx, int *total)             { rec.time++; rec.last_ctx = ctx; if (total) *total = 240000; return 61000; }
static const char *cb_path(void *ctx, BROWSE_ENTRY *b) { rec.path++; rec.last_ctx = ctx; rec.last_br = b; return CURRENT_PATH; }
static int  cb_seek(void *ctx, int t, int p)           { rec.seek++; rec.last_ctx = ctx; rec.last_seek_time = t; rec.last_seek_pos = p; return 222; }

static AV_CALLBACKS full_cbs = { cb_stop, cb_pause, cb_un_pause, cb_is_paused, cb_time, cb_path, cb_seek };
static AV_CALLBACKS empty_cbs;		/* every function pointer NULL */

static void reset(void)
{
	memset(&rec, 0, sizeof rec);
	AV_set_state(AV_STOPPED, 0, 0, NULL, NULL);
}

static void play_with(AV_CALLBACKS *cbs)
{
	reset();
	AV_set_state(AV_PLAYING, 7, 3, &my_ctx, cbs);
}

static ID3_TAG make_tag(void)
{
	ID3_TAG t;
	memset(&t, 0, sizeof t);
	t.valid = 1;
	strcpy(t.title, "Title");
	strcpy(t.artist, "Artist");
	t.track = 9;
	t.msec = 123456;
	return t;
}

/* ---- state ---------------------------------------------------------------------------------- */

TEST(initial_state_is_stopped_with_nothing_registered)
{
	CHECK_EQ_INT(AV_get_state(), AV_STOPPED);
	CHECK_EQ_INT(AV_get_type(), 0);
	CHECK_EQ_INT(AV_get_power_veto(), 0);
	CHECK_NULL(AV_get_ctx());
	CHECK_EQ_INT(AV_get_fs(), DEV_UNAVAIL);
}

TEST(set_state_playing_records_everything_and_vetoes_power_down)
{
	reset();
	AV_set_state(AV_PLAYING, 5, 2, &my_ctx, &full_cbs);
	CHECK_EQ_INT(AV_get_state(), AV_PLAYING);
	CHECK_EQ_INT(AV_get_type(), 5);
	CHECK_EQ_INT(AV_get_power_veto(), 1);
	CHECK(AV_get_ctx() == &my_ctx);
	CHECK_EQ_INT(AV_get_fs(), 2);
}

TEST(set_state_stopped_clears_the_power_veto_and_hides_the_filesystem)
{
	play_with(&full_cbs);
	CHECK_EQ_INT(AV_get_power_veto(), 1);
	AV_set_state(AV_STOPPED, 0, 4, NULL, NULL);
	CHECK_EQ_INT(AV_get_state(), AV_STOPPED);
	CHECK_EQ_INT(AV_get_power_veto(), 0);
	CHECK_NULL(AV_get_ctx());
	CHECK_EQ_INT(AV_get_fs(), DEV_UNAVAIL);		/* fs is only reported while playing */
}

TEST(any_non_stopped_state_is_treated_as_playing_for_the_power_veto)
{
	reset();
	AV_set_state(99, 0, 0, NULL, NULL);
	CHECK_EQ_INT(AV_get_power_veto(), 1);
}

TEST(set_state_discards_the_previous_tag)
{
	ID3_TAG t = make_tag(), out;
	AV_set_tag(&t);
	AV_set_state(AV_PLAYING, 1, 1, &my_ctx, &full_cbs);
	memset(&out, 0xCC, sizeof out);
	AV_get_tag(&out);
	CHECK_EQ_INT(out.valid, 0);
	CHECK_EQ_STR(out.title, "");
	CHECK_EQ_INT(out.track, 0);
}

/* ---- tag ------------------------------------------------------------------------------------- */

TEST(tag_is_stored_by_value_and_returned_intact)
{
	ID3_TAG t = make_tag(), out;
	AV_set_tag(&t);
	strcpy(t.title, "changed afterwards");			/* the registry must hold a copy */
	memset(&out, 0, sizeof out);
	AV_get_tag(&out);
	CHECK_EQ_INT(out.valid, 1);
	CHECK_EQ_STR(out.title, "Title");
	CHECK_EQ_STR(out.artist, "Artist");
	CHECK_EQ_INT(out.track, 9);
	CHECK_EQ_INT(out.msec, 123456);
}

TEST(setting_a_null_tag_clears_it)
{
	ID3_TAG t = make_tag(), out;
	AV_set_tag(&t);
	AV_set_tag(NULL);
	memset(&out, 0xCC, sizeof out);
	AV_get_tag(&out);
	CHECK_EQ_INT(out.valid, 0);
	CHECK_EQ_STR(out.title, "");
}

TEST(getting_a_tag_into_null_is_a_no_op)
{
	ID3_TAG t = make_tag();
	AV_set_tag(&t);
	CHECK_SURVIVES(AV_get_tag(NULL));
}

/* ---- delegation: pause / un_pause / seek / is_paused / time / path --------------------------- */

TEST(pause_forwards_to_the_client_with_its_context_and_returns_its_result)
{
	play_with(&full_cbs);
	CHECK_EQ_INT(AV_pause(), 111);
	CHECK_EQ_INT(rec.pause, 1);
	CHECK(rec.last_ctx == &my_ctx);
}

TEST(un_pause_forwards_the_was_paused_flag)
{
	play_with(&full_cbs);
	AV_un_pause(1);
	CHECK_EQ_INT(rec.un_pause, 1);
	CHECK_EQ_INT(rec.last_unpause_arg, 1);
	AV_un_pause(0);
	CHECK_EQ_INT(rec.un_pause, 2);
	CHECK_EQ_INT(rec.last_unpause_arg, 0);
	CHECK(rec.last_ctx == &my_ctx);
}

TEST(seek_forwards_time_and_position_and_returns_the_client_result)
{
	play_with(&full_cbs);
	CHECK_EQ_INT(AV_seek(90000, 42), 222);
	CHECK_EQ_INT(rec.seek, 1);
	CHECK_EQ_INT(rec.last_seek_time, 90000);
	CHECK_EQ_INT(rec.last_seek_pos, 42);
	CHECK(rec.last_ctx == &my_ctx);
}

TEST(is_paused_reflects_the_client_state)
{
	play_with(&full_cbs);
	CHECK_EQ_INT(AV_is_paused(), 0);
	AV_pause();
	CHECK_EQ_INT(AV_is_paused(), 1);
	AV_un_pause(1);
	CHECK_EQ_INT(AV_is_paused(), 0);
	CHECK_EQ_INT(rec.is_paused, 3);
}

TEST(current_time_reports_position_and_total_from_the_client)
{
	play_with(&full_cbs);
	int total = -1;
	CHECK_EQ_INT(AV_get_current_time(&total), 61000);
	CHECK_EQ_INT(total, 240000);
	CHECK_EQ_INT(AV_get_current_time(NULL), 61000);	/* total is optional for the caller */
	CHECK_EQ_INT(rec.time, 2);
}

TEST(current_path_returns_the_clients_path_and_passes_the_browse_entry)
{
	play_with(&full_cbs);
	BROWSE_ENTRY *br = (BROWSE_ENTRY *)0x1234;	/* opaque to ac_av: only forwarded */
	CHECK(AV_get_current_path(br) == CURRENT_PATH);
	CHECK(rec.last_br == br);
	CHECK(AV_get_current_path(NULL) == CURRENT_PATH);
}

TEST(nothing_is_forwarded_while_stopped)
{
	reset();
	AV_set_state(AV_STOPPED, 0, 0, &my_ctx, &full_cbs);	/* callbacks registered but not playing */
	int total = 77;
	CHECK_EQ_INT(AV_pause(), 0);
	AV_un_pause(1);
	CHECK_EQ_INT(AV_seek(1, 2), 0);
	CHECK_EQ_INT(AV_is_paused(), 0);
	CHECK_EQ_INT(AV_get_current_time(&total), 0);
	CHECK_EQ_INT(total, 0);				/* cleared, not left stale */
	CHECK_NULL(AV_get_current_path(NULL));
	CHECK_EQ_INT(AV_get_current_time(NULL), 0);
	CHECK_EQ_INT(rec.pause + rec.un_pause + rec.seek + rec.is_paused + rec.time + rec.path, 0);
}

TEST(playing_without_any_callback_table_answers_neutral_values)
{
	play_with(NULL);
	int total = 55;
	CHECK_EQ_INT(AV_pause(), 0);
	AV_un_pause(1);
	CHECK_EQ_INT(AV_seek(1, 2), 0);
	CHECK_EQ_INT(AV_is_paused(), 0);
	CHECK_EQ_INT(AV_get_current_time(&total), 0);
	CHECK_EQ_INT(total, 0);
	CHECK_NULL(AV_get_current_path(NULL));
}

TEST(playing_with_an_empty_callback_table_answers_neutral_values)
{
	play_with(&empty_cbs);
	int total = 55;
	CHECK_EQ_INT(AV_pause(), 0);
	AV_un_pause(1);
	CHECK_EQ_INT(AV_seek(1, 2), 0);
	CHECK_EQ_INT(AV_is_paused(), 0);
	CHECK_EQ_INT(AV_get_current_time(&total), 0);
	CHECK_EQ_INT(total, 0);
	CHECK_NULL(AV_get_current_path(NULL));
	CHECK_SURVIVES(AV_stop());
}

/* ---- stop ------------------------------------------------------------------------------------ */

TEST(stop_asks_the_client_to_stop_with_resume_allowed_and_leaves_the_state_to_it)
{
	play_with(&full_cbs);
	AV_stop();
	CHECK_EQ_INT(rec.stop, 1);
	CHECK_EQ_INT(rec.last_stop_arg, TRUE);
	CHECK(rec.last_ctx == &my_ctx);
	/* the client is responsible for calling AV_set_state(AV_STOPPED...) itself */
	CHECK_EQ_INT(AV_get_state(), AV_PLAYING);
}

TEST(stop_without_a_client_stop_handler_stops_the_registry_itself)
{
	play_with(&empty_cbs);
	AV_stop();
	CHECK_EQ_INT(AV_get_state(), AV_STOPPED);
	CHECK_EQ_INT(AV_get_type(), 0);
	CHECK_EQ_INT(AV_get_power_veto(), 0);
	CHECK_NULL(AV_get_ctx());

	play_with(NULL);
	AV_stop();
	CHECK_EQ_INT(AV_get_state(), AV_STOPPED);
}

TEST(stop_when_already_stopped_does_nothing)
{
	reset();
	AV_set_state(AV_STOPPED, 3, 0, &my_ctx, &full_cbs);
	AV_stop();
	CHECK_EQ_INT(rec.stop, 0);
	CHECK_EQ_INT(AV_get_type(), 3);			/* record untouched */
	CHECK(AV_get_ctx() == &my_ctx);
}

TEST(stop_set_resume_passes_the_resume_flag_through)
{
	play_with(&full_cbs);
	AV_stop_set_resume(FALSE);
	CHECK_EQ_INT(rec.stop, 1);
	CHECK_EQ_INT(rec.last_stop_arg, FALSE);
	AV_stop_set_resume(TRUE);
	CHECK_EQ_INT(rec.stop, 2);
	CHECK_EQ_INT(rec.last_stop_arg, TRUE);
}

TEST(stop_set_resume_without_a_handler_stops_the_registry_and_when_stopped_does_nothing)
{
	play_with(&empty_cbs);
	AV_stop_set_resume(FALSE);
	CHECK_EQ_INT(AV_get_state(), AV_STOPPED);

	reset();
	AV_set_state(AV_STOPPED, 3, 0, &my_ctx, &full_cbs);
	AV_stop_set_resume(TRUE);
	CHECK_EQ_INT(rec.stop, 0);
	CHECK_EQ_INT(AV_get_type(), 3);
}

TEST(stopping_the_android_player_has_no_effect_on_the_registry)
{
	play_with(&full_cbs);
	AV_stop_android_player();
	CHECK_EQ_INT(AV_get_state(), AV_PLAYING);
	CHECK_EQ_INT(rec.stop, 0);
}

/* ---- debug commands, through the real dispatcher -------------------------------------------- */

TEST(debug_command_avs_stops_playback)
{
	play_with(&full_cbs);
	debug_do_cmd((const unsigned char *)"avs");
	CHECK_EQ_INT(rec.stop, 1);
}

TEST(debug_command_avp_toggles_between_pause_and_resume)
{
	play_with(&full_cbs);
	debug_do_cmd((const unsigned char *)"avp");
	CHECK_EQ_INT(rec.pause, 1);
	CHECK_EQ_INT(rec.paused, 1);
	debug_do_cmd((const unsigned char *)"avp");
	CHECK_EQ_INT(rec.un_pause, 1);
	CHECK_EQ_INT(rec.last_unpause_arg, 0);
	CHECK_EQ_INT(rec.paused, 0);
}

TEST(debug_command_avt_queries_the_current_time)
{
	play_with(&full_cbs);
	debug_do_cmd((const unsigned char *)"avt");
	CHECK_EQ_INT(rec.time, 1);
}
