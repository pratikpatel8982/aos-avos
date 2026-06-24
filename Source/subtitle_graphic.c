/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "global.h"
#include "types.h"
#include "astdlib.h"
#include "debug.h"
#include "subtitle.h"
#include "app_video.h"
#include "image.h"

#include "rect.h"
#include "stream.h"

#define DBG if(Debug[DBG_SUB])
#define ERR if(1)

#ifdef CONFIG_SUBTITLES

static int sub_num = 0;
static VIDEO_FRAME *current = NULL;
static int remove_time;

static AV_PROPERTIES *av_props = NULL;

static int lang_index = 0;

void subtitles_graphic_set_lang( int index )
{
	if( !av_props ) {
		// No subtitles in stream
ERR serprintf("%s invalid stream\n",__FUNCTION__);
		return;
	}
	lang_index = index;

	// clear old subs
	subtitles_text_update( "", "" );
	subtitles_graphic_update( NULL );

	Video_SetSubtitleStream( lang_index );

	if( av_props->subs != lang_index ) {
DBG serprintf("Subtitle stream change failed\n");
	}
}

int subtitles_graphic_open( AV_PROPERTIES *av )
{
DBG serprintf("%s: %d\r\n", __FUNCTION__, av->subs_max );
	av_props    = av;
	sub_num     = av->subs_max;
	remove_time = -1;

	if( av->subs_max ) {
		return 0;
	}

	return 1;
}

int subtitles_graphic_get_count( void )
{
	if( !av_props ) {
		return 0;
	}
	return av_props->subs_max;
}

const char *subtitles_graphic_get_item( int index, int *text )
{
	if( text )
		*text = 0;
		
	if( !av_props || index > av_props->subs_max ) {
		return "";
	}
	if( av_props->sub[index ].valid ) {
		if( text )
			*text = !av_props->sub[index].gfx;
		return av_props->sub[index].name;
	} else {
		return "";
	}
}

void subtitles_graphic_changed( VIDEO_FRAME *subs )
{
DBG serprintf("subtitles_graphic_changed: ");
	current = subs;
	if( current ) {
DBG		serprintf("%8d/%5d  %d x %d", current->time, current->duration, current->window.width, current->window.height);
	}
DBG	serprintf("\r\n");
}

void subtitles_graphic_close( void )
{
DBG serprintf("subtitles_graphic_close\r\n");
	sub_num = 0;
	current = NULL;
	av_props = NULL;
}

void subtitles_graphic_display( int time )
{
	if( time == -1 || ( remove_time != -1 && time > remove_time ) ) {
		// remove all subs
		if (g_sub_engine && av_props && av_props->sub[av_props->subs].gfx) {
			// Tell the new C-Engine to clear the screen
			sub_engine_flush(g_sub_engine);
		} else {
			// Fallback for extreme edge cases, though Java UI should be blocked
			subtitles_graphic_update( NULL );
			subtitles_text_update( "", "" );
		}
		remove_time = -1;
	}

	if( !av_props || !sub_num || !current ) {
		return;
	}

	if( time < current->time ) {
		return; // too early
	} else if( time > current->time + current->duration ) {
		current = NULL;
		return; // too late, drop it
	}

	// show it!
	if( av_props->sub[av_props->subs].gfx ) {
		IMAGE cropped = image_crop( (IMAGE *)current, &current->window );

		// --- NATIVE OPENGL UPGRADE ---
		// Bypass the old Java SubtitleGfxView entirely!
		if (g_sub_engine) {
			sub_engine_feed_bitmap(g_sub_engine,
								   cropped.data[0],
						  cropped.width,
						  cropped.height,
						  cropped.linestep[0],
						  current->colorspace,
						  current->window.x,  // Original X offset
						  current->window.y,  // Original Y offset
						  current->time,
						  current->duration);
		}
	} else {
		// Text is already handled by stream_subtitle.c Fast Lane!
		// We do absolutely nothing here for text.
	}

	remove_time = current->time + current->duration;
	current = NULL;
}

#endif	// CONFIG_SUBTITLES
