/*
 * test_dts.c -- unit tests for Source/dts.c (DTS profile -> WAVE format mapping).
 *
 * Public API only: the real instrumented dts.o is linked. The profile numbers are the ones FFmpeg
 * reports in AVCodecParameters.profile, so the tests take them from the real libavcodec header
 * instead of re-typing them: if FFmpeg renumbers a profile this suite notices.
 */
#include "avos_test.h"
#include "av.h"
#include "dts.h"

#include <libavcodec/defs.h>

TEST(core_dts_and_dts_es_are_plain_dts)
{
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS), WAVE_FORMAT_DTS);
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_ES), WAVE_FORMAT_DTS);
}

TEST(lossless_and_express_profiles_map_to_dts_hd_ma)
{
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_HD_MA), WAVE_FORMAT_DTS_HD_MA);
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_HD_MA_X), WAVE_FORMAT_DTS_HD_MA);
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_HD_MA_X_IMAX), WAVE_FORMAT_DTS_HD_MA);
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_EXPRESS), WAVE_FORMAT_DTS_HD_MA);
}

TEST(high_resolution_profiles_map_to_dts_hd)
{
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_96_24), WAVE_FORMAT_DTS_HD);
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_HD_HRA), WAVE_FORMAT_DTS_HD);
}

TEST(unknown_or_unset_profiles_fall_back_to_plain_dts)
{
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_UNKNOWN), WAVE_FORMAT_DTS);	/* -99 */
	CHECK_EQ_INT(DTS_get_format_from_profile(0), WAVE_FORMAT_DTS);
	CHECK_EQ_INT(DTS_get_format_from_profile(AV_PROFILE_DTS_HD_MA + 1000), WAVE_FORMAT_DTS);
	CHECK_EQ_INT(DTS_get_format_from_profile(-1), WAVE_FORMAT_DTS);
}

TEST(the_three_wave_formats_are_distinct)
{
	CHECK(WAVE_FORMAT_DTS != WAVE_FORMAT_DTS_HD);
	CHECK(WAVE_FORMAT_DTS != WAVE_FORMAT_DTS_HD_MA);
	CHECK(WAVE_FORMAT_DTS_HD != WAVE_FORMAT_DTS_HD_MA);
}
