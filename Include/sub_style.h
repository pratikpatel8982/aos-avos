/*
 * sub_style.h — User-configurable subtitle appearance settings.
 *
 * This is the C-side mirror of Nova's existing subtitle settings UI
 * (color, background, size, opacity, etc.) — previously Java-side
 * preferences consumed ad-hoc by SubtitleTextView/SubtitleGfxView.
 * Now it's a single struct, owned natively, with explicit setters
 * exposed to JNI so the existing settings screens keep working
 * unchanged — they just call down into C instead of setting Java
 * View properties directly.
 *
 * Applies to SUB_EVENT_TEXT events (SRT, plain SSA fallback) only.
 * Bitmap events (libass-rendered ASS, VOBSUB, PGS) are pre-rendered
 * by their own backend and only respect the subset that makes sense:
 *   - libass: font size override maps to ass_renderer scaling, see
 *     sub_format_ssa.h's apply_user_style()
 *   - VOBSUB/PGS: only global opacity/scale apply (no per-glyph color,
 *     since the bitmap is already rasterized upstream)
 */

#pragma once

#include "sub_types.h"

/* ------------------------------------------------------------------
 * Global, persisted user style — one instance per player session.
 * Thread-safety: all setters take an internal lock; the renderer reads
 * a snapshot copy at the start of each frame, never a live pointer.
 * ------------------------------------------------------------------ */

typedef struct SUB_USER_STYLE SUB_USER_STYLE;  /* opaque handle */

/* Create / destroy. One instance lives for the life of the player. */
SUB_USER_STYLE *sub_style_create(void);
void            sub_style_destroy(SUB_USER_STYLE *style);

/* ------------------------------------------------------------------
 * Setters — each corresponds to an existing Nova subtitle setting.
 * Safe to call from any thread (Java UI thread typically); takes
 * effect on the next rendered frame.
 * ------------------------------------------------------------------ */

void sub_style_set_text_color(SUB_USER_STYLE *style, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void sub_style_set_outline_color(SUB_USER_STYLE *style, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void sub_style_set_outline_width(SUB_USER_STYLE *style, float px);

void sub_style_set_bg_enabled(SUB_USER_STYLE *style, int enabled);
void sub_style_set_bg_color(SUB_USER_STYLE *style, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void sub_style_set_bg_opacity(SUB_USER_STYLE *style, float opacity /* 0..1 */);

void sub_style_set_font_size(SUB_USER_STYLE *style, float pt);
void sub_style_set_font_scale(SUB_USER_STYLE *style, float scale /* 1.0 = 100% */);
void sub_style_set_font_family(SUB_USER_STYLE *style, const char *family_name);
void sub_style_set_bold(SUB_USER_STYLE *style, int bold);
void sub_style_set_italic(SUB_USER_STYLE *style, int italic);

/* Vertical position bias, e.g. "subtitle position" slider (0 = bottom
 * default, positive = shift up), in fraction of video height. */
void sub_style_set_vertical_offset(SUB_USER_STYLE *style, float fraction);

/* Master toggle a user might flip per-format: "use embedded ASS styling
 * vs always apply my custom style" — when forced, libass output is
 * still used for layout/animation but recolored/rescaled per style. */
void sub_style_set_force_override(SUB_USER_STYLE *style, int force);

/* ------------------------------------------------------------------
 * Snapshot read — used internally by sub_engine.c at frame-build time.
 * Returns a copy (not a pointer into live state) so the renderer never
 * races with a setter call from the UI thread.
 * ------------------------------------------------------------------ */
SUB_STYLE sub_style_snapshot(const SUB_USER_STYLE *style);

/* Returns 1 if force_override is set (see sub_style_set_force_override). */
int sub_style_is_forced(const SUB_USER_STYLE *style);
