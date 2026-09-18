/*
 * sub_types.h — Core data types shared across the entire subtitle engine.
 *
 * Every format backend (SRT, SSA/libass, VOBSUB, PGS) normalizes its
 * output into SUB_FRAME before handing it to the renderer. This is the
 * single seam the whole redesign pivots on: one renderer, N format
 * backends, all producing the same intermediate shape.
 */

#pragma once

#include <stdint.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------
 * Color / style primitives
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t r, g, b, a;
} SUB_COLOR;

/* Text styling — applies to glyph-run events (SRT, plain SSA dialogue
 * when not using libass's own override tags; also the basis for user
 * override settings that get merged on top of whatever the format
 * specifies, see sub_style.h). */
typedef struct {
    SUB_COLOR   fg;             /* glyph fill color                     */
    SUB_COLOR   bg;             /* background box color (if bg_enabled) */
    SUB_COLOR   outline;        /* outline/border color                 */
    float       font_size_pt;   /* nominal size, scaled by renderer      */
    float       outline_width;  /* px, 0 = no outline                    */
    float       bg_opacity;     /* 0..1, independent of bg.a for UI convenience */
    int         bg_enabled;     /* draw background box behind text       */
    int         bold;
    int         italic;
    char        font_family[64];/* used by text backends; ignored by libass
                                  * (libass owns its own font selection) */
} SUB_STYLE;

/* ------------------------------------------------------------------
 * SUB_EVENT — one renderable "thing" at a point in time.
 *
 * Two kinds:
 *   SUB_EVENT_TEXT   — glyph run(s) to be shaped/rasterized by the
 *                       renderer itself (SRT, plain-text SSA fallback).
 *                       The renderer owns font shaping for this kind.
 *   SUB_EVENT_BITMAP — pre-rendered RGBA pixels, already positioned.
 *                       Used by libass (ass_render_frame output),
 *                       VOBSUB, and PGS. The renderer just uploads and
 *                       draws a textured quad — no shaping involved.
 * ------------------------------------------------------------------ */

typedef enum {
    SUB_EVENT_TEXT   = 0,
    SUB_EVENT_BITMAP = 1,
} SUB_EVENT_KIND;

/* Pixel payload shape carried by a SUB_EVENT_BITMAP event.
 *   SUB_BITMAP_RGBA8   — straight (non-premultiplied) RGBA, w*h*4 bytes,
 *                         color already baked into the pixels. Used by
 *                         sub_format_gfx.c (PGS/DVD — FFmpeg hands back
 *                         real colored imagery, there's nothing to tint).
 *   SUB_BITMAP_MASK_R8 — 8-bit coverage mask, w*h*1 bytes. `color` carries
 *                         the RGB + alpha to tint the mask with; the
 *                         renderer multiplies mask*color (fragment shader
 *                         for GL, or directly in the CPU 3D bridge) instead
 *                         of the format backend pre-expanding every pixel
 *                         to RGBA on the CPU. Used by sub_format_ssa.c —
 *                         libass hands back ASS_Image as an 8-bit mask plus
 *                         one color per image, and expanding that to RGBA
 *                         before it reaches the GPU just re-does work
 *                         libass already did the hard part of.
 *   SUB_BITMAP_BGRA8   — codec_ffsub's native BGRA byte order, w*h*4 bytes,
 *                         color already baked into the pixels (same as
 *                         RGBA8, just B/R swapped in memory). Used by
 *                         sub_format_gfx.c when it can skip the CPU
 *                         BGRA->RGBA swizzle entirely: on a device that
 *                         supports GL_EXT_texture_format_BGRA8888, the GPU
 *                         samples this layout correctly with no shader
 *                         changes, so the format backend just hands the
 *                         decoder's bytes through untouched. The renderer
 *                         decides per-device (see sub_render_gl.c's
 *                         has_bgra_ext) whether to upload it directly or
 *                         fall back to a one-time CPU swizzle; either way,
 *                         anything reading these pixels directly (the CPU
 *                         3D-bridge blend in sub_render_gl_fill_bitmap())
 *                         must swap R/B itself, same as the GL fallback
 *                         does -- this format is never silently equivalent
 *                         to RGBA8 to a consumer that doesn't check for it.
 */
typedef enum {
    SUB_BITMAP_RGBA8   = 0,
    SUB_BITMAP_MASK_R8 = 1,
    SUB_BITMAP_BGRA8   = 2,
} SUB_BITMAP_FORMAT;

typedef struct SUB_EVENT {
    SUB_EVENT_KIND kind;

    /* Placement, in video-frame pixel space (renderer maps to surface space) */
    int x, y, w, h;

    union {
        struct {
            const char *utf8_text;   /* owned by the event, freed with it */
            SUB_STYLE   style;       /* resolved style: format defaults
                                       * merged with user settings        */
        } text;

        struct {
            SUB_BITMAP_FORMAT format;  /* which payload shape this event carries */
            const uint8_t    *pixels;  /* RGBA8/BGRA8: tightly packed w*h*4 bytes
                                         * (channel order per `format`).
                                         * MASK_R8: tightly packed w*h*1 coverage bytes. */
            int               stride;  /* bytes per row (may be > w*bpp)    */
            SUB_COLOR         color;   /* MASK_R8 only — RGB + alpha to tint
                                         * the mask with. Unused/zeroed for
                                         * RGBA8/BGRA8, where color already
                                         * lives in the pixels themselves.  */
            /* Ownership: the event owns this buffer and frees it in
             * sub_frame_unref() (sub_engine.c). The renderer/CPU bridge may
             * hold a ref on the whole SUB_FRAME (sub_frame_ref()) for as
             * long as they're actively reading these pixels, but never
             * retain the raw pointer beyond that. (Corrected from the prior
             * comment here, which referenced a sub_engine_submit_frame()
             * that doesn't exist in this codebase — the real lifetime is
             * the refcount/pin scheme visible in sub_render_gl.c and
             * sub_engine.c.) */
        } bitmap;
    } data;

    struct SUB_EVENT *next;  /* multiple simultaneous events per frame (e.g.
                               * top+bottom lines, or multi-region VOBSUB) */
} SUB_EVENT;

/* ------------------------------------------------------------------
 * SUB_FRAME — everything to display at a given PTS.
 * Produced by a format backend, consumed by the renderer.
 * ------------------------------------------------------------------ */

typedef struct {
    _Atomic int refcount;
    int64_t     pts_ms;
    int64_t     duration_ms;   /* renderer hint only; for animated formats
                                 * (libass) this may be a short "valid until
                                 * next submit" window rather than the cue's
                                 * full duration                            */
    int         video_w;       /* reference frame size events are placed in */
    int         video_h;
    SUB_EVENT  *events;        /* linked list, NULL = nothing to show       */
} SUB_FRAME;

/* ------------------------------------------------------------------
 * SUB_FMT_ID — engine backend selector. NOT the same thing as av.h's
 * SUB_FORMAT_* (SUB_FORMAT_SSA, SUB_FORMAT_PGS, SUB_FORMAT_WEBVTT, ...),
 * which identifies the on-disk/container codec format (12 values, used
 * for demux dispatch and codec_ffsub decoder selection). SUB_FMT_ID is
 * the much smaller set of engine backends those 12 formats collapse
 * onto (3 values) — e.g. SUB_FORMAT_WEBVTT, SUB_FORMAT_MOV_TEXT, and
 * SUB_FORMAT_TEXT are all plain text and all map to SUB_FMT_SRT.
 * There is no numeric relationship between the two enums; the mapping
 * from SUB_FORMAT_* to SUB_FMT_ID is semantic and lives in exactly one
 * place: sub_fmt_from_format() in sub_format.h/.c. Do not re-derive it
 * ad hoc at call sites.
 * ------------------------------------------------------------------ */

typedef enum {
    SUB_FMT_SRT     = 0,
    SUB_FMT_SSA     = 1,   /* via libass */
    SUB_FMT_GFX     = 2,   /* NEW: Universal OpenGL Bitmap Backend */
    SUB_FMT_UNKNOWN = -1,
} SUB_FMT_ID;
