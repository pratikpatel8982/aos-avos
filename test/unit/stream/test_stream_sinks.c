/*
 * test_stream_sinks.c -- the audio sink vtables used by the stream layer.
 *
 * Found by the whole-code audit (clang-tidy NullDereference / gcc -fanalyzer "called function pointer is null"):
 * generic stream code calls sink members through s->audio_sink without checking them, so every sink table must
 * fill every member. The real sink does; the fake sink (debug toggle "sfas") stops after `mute`.
 *
 * Real code only: both tables are the real objects from libavos_cov.a. Each table is read as an array of
 * pointers after its name, so a member added to STREAM_SINK_AUDIO later is checked automatically.
 */
#include "avos_test.h"
#include "stream.h"

extern STREAM_SINK_AUDIO stream_sink_audio;
extern STREAM_SINK_AUDIO stream_sink_audio_FAKE;

#define NMEMBERS ((int)((sizeof(STREAM_SINK_AUDIO) - offsetof(STREAM_SINK_AUDIO, open)) / sizeof(void *)))

static int null_members(const STREAM_SINK_AUDIO *t)
{
	const void *const *m = (const void *const *)&t->open;
	int n = 0;
	for (int i = 0; i < NMEMBERS; i++)
		n += (m[i] == NULL);
	return n;
}

TEST(the_real_audio_sink_provides_every_member)
{
	CHECK_EQ_STR(stream_sink_audio.name, "audio");
	CHECK_EQ_INT(null_members(&stream_sink_audio), 0);
}

TEST(the_default_sink_selection_returns_the_real_sink_unless_the_debug_toggle_is_on)
{
	CHECK(stream_get_default_audio_sink() == &stream_sink_audio);
}

/*
 * KNOWN DEFECT (Source/stream_sink_audio_fake.c:245): the initializer ends at `mute`, so set_vol and
 * get_session_id are NULL. stream_get_audio_session_id() (stream_video.c:6028) calls
 * s->audio_sink->get_session_id(s) with no NULL check, so it crashes when the fake sink is active
 * (debug toggle "sfas"). The fix is to add both members to the fake table; this test then fails on purpose
 * and must be turned into a "provides every member" check like the one above.
 */
TEST(known_defect_the_fake_audio_sink_leaves_set_vol_and_get_session_id_null)
{
	CHECK_EQ_STR(stream_sink_audio_FAKE.name, "audio fake");
	CHECK_NULL(stream_sink_audio_FAKE.set_vol);
	CHECK_NULL(stream_sink_audio_FAKE.get_session_id);
	CHECK_EQ_INT(null_members(&stream_sink_audio_FAKE), 2);
	CHECK_NOT_NULL(stream_sink_audio_FAKE.mute);		/* everything before them is filled */
}
