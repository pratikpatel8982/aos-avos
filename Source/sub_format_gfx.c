#include "sub_engine.h"
#include "stream.h"   // for AV_IMAGE_BGRA_32
#include <stdlib.h>
#include <string.h>

typedef struct {
    SUB_FRAME *current_frame; // owned here — never freed by the GL renderer
    int        is_cleared;    // 1 = no subtitle currently visible (PGS clear signal received)
    int        is_dirty;
    int        canvas_w, canvas_h;           // on-screen GL surface size. NOT the space
                                              // ev->x/y/w/h are expressed in -- see below.
    int        real_video_w, real_video_h;   // decoded video's own coded size -- fixed for
                                              // the track's lifetime. THIS is the space
                                              // codec_ffsub's x_offset/y_offset/width/height
                                              // are expressed in.
    int        video_box_x, video_box_y;     // where the video's own on-screen box sits
    int        video_box_w, video_box_h;     // within the canvas (post letterbox/pillarbox/
                                              // zoom-crop/stretch), as reported by
                                              // sub_engine_set_video_box().
} GFX_BACKEND;

// ---------------------------------------------------------------------------
// gfx_open
// ---------------------------------------------------------------------------
static int gfx_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    GFX_BACKEND *ctx = calloc(1, sizeof(GFX_BACKEND));
    ctx->canvas_w     = params->video_w > 0 ? params->video_w : 1920;
    ctx->canvas_h     = params->video_h > 0 ? params->video_h : 1080;
    ctx->real_video_w = params->real_video_w > 0 ? params->real_video_w : ctx->canvas_w;
    ctx->real_video_h = params->real_video_h > 0 ? params->real_video_h : ctx->canvas_h;
    // Until sub_engine_set_video_box() has reported an actual box, assume the video fills
    // the canvas 1:1 -- this is today's (buggy) behavior, kept as the fallback so a track
    // opened before Java's first box report still renders (just not correctly positioned
    // until the first real report arrives, same as before this fix).
    ctx->video_box_x  = params->video_box_w > 0 ? params->video_box_x : 0;
    ctx->video_box_y  = params->video_box_h > 0 ? params->video_box_y : 0;
    ctx->video_box_w  = params->video_box_w > 0 ? params->video_box_w : ctx->canvas_w;
    ctx->video_box_h  = params->video_box_h > 0 ? params->video_box_h : ctx->canvas_h;
    ctx->is_cleared = 1; // nothing to show yet
    be->priv = ctx;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_feed_bitmap
//
// Called by _feed_bitmap_to_engine() in stream_subtitle.c every time
// codec_ffsub produces a decoded frame — including the PGS zero-rect clear
// frame (width=1, height=1, duration=0).
//
// Colorspace:
//   codec_ffsub always produces AV_IMAGE_BGRA_32 (confirmed by the
//   av_image_alloc(AV_PIX_FMT_BGRA) call and frame->colorspace assignment).
//   The GL renderer expects RGBA. We swizzle R<->B during the pixel copy.
// ---------------------------------------------------------------------------
static int gfx_feed_bitmap(SUB_FORMAT_BACKEND *be,
                           uint8_t *pixels,
                           int width, int height, int pitch,
                           int colorspace,
                           int x_offset, int y_offset,
                           int64_t pts_ms, int64_t duration_ms)
{
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;

    // Retire the previous stored frame before replacing it
    if (ctx->current_frame) {
        sub_frame_unref(ctx->current_frame);
        ctx->current_frame = NULL;
    }

    // PGS clear signal: codec_ffsub sends a 1x1 zero-rect frame with
    // duration=0 to signal "hide the current subtitle". Honour it.
    if (duration_ms == 0 || !pixels || width <= 0 || height <= 0) {
        ctx->is_cleared = 1;
        ctx->is_dirty = 1;
        return 0;
    }

    ctx->is_cleared = 0;
    ctx->is_dirty = 1;

    // Build the stored frame
    SUB_FRAME *frame  = calloc(1, sizeof(SUB_FRAME));
    atomic_init(&frame->refcount, 1);

    frame->pts_ms      = pts_ms;
    frame->duration_ms = duration_ms;
    frame->video_w        = ctx->canvas_w;
    frame->video_h        = ctx->canvas_h;
    frame->real_video_w   = ctx->real_video_w;
    frame->real_video_h   = ctx->real_video_h;
    frame->video_box_x    = ctx->video_box_x;
    frame->video_box_y    = ctx->video_box_y;
    frame->video_box_w    = ctx->video_box_w;
    frame->video_box_h    = ctx->video_box_h;

    SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
    ev->kind             = SUB_EVENT_BITMAP;
    ev->x                = x_offset;
    ev->y                = y_offset;
    ev->w                = width;
    ev->h                = height;
    ev->data.bitmap.stride = width * 4; // always RGBA after swizzle

    uint8_t *rgba = malloc(width * height * 4);
    ev->data.bitmap.rgba = rgba;

    const int is_bgra = (colorspace == AV_IMAGE_BGRA_32);

    for (int row = 0; row < height; row++) {
        const uint8_t *src = pixels + row * pitch;
        uint8_t       *dst = rgba   + row * (width * 4);
        for (int col = 0; col < width; col++) {
            if (is_bgra) {
                // BGRA -> RGBA: swap B(src[0]) and R(src[2])
                dst[0] = src[2]; // R
                dst[1] = src[1]; // G
                dst[2] = src[0]; // B
                dst[3] = src[3]; // A
            } else {
                // Already RGBA, just copy
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
            src += 4;
            dst += 4;
        }
    }

    frame->events      = ev;
    ctx->current_frame = frame;

    return 0;
}

// ---------------------------------------------------------------------------
// gfx_render_at
//
// The GL renderer polls this every frame.  For bitmap subtitles there is no
// per-frame re-render — we just hand back a clone of whatever codec_ffsub
// last gave us, until a clear signal arrives.
//
// We return a CLONE (not the stored pointer) because the GL renderer will
// call free_frame on the returned pointer.  ctx->current_frame must survive
// intact for the next poll.
//
// We do NOT do a time-window check here.  PGS subtitles don't carry reliable
// duration; the real end is signalled by the zero-rect clear packet handled
// in gfx_feed_bitmap above.  VobSub does have durations but they are also
// unreliable — keeping it simple: show until cleared.
// ---------------------------------------------------------------------------
static SUB_FRAME *gfx_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;

    // 1. If nothing changed, return NULL (Triggers the GL Bypass)
    if (!ctx->is_dirty) return NULL;
    // Consume the dirty flag
    ctx->is_dirty = 0;
    // 2. If it changed to a CLEAR state, return an empty frame to wipe the screen
    if (ctx->is_cleared || !ctx->current_frame) {
        SUB_FRAME *empty_frame = calloc(1, sizeof(SUB_FRAME));
        atomic_init(&empty_frame->refcount, 1);
        return empty_frame; // No events attached = clear screen
    }

    sub_frame_ref(ctx->current_frame);
    return ctx->current_frame;
}

// ---------------------------------------------------------------------------
// gfx_free_frame
//
// Called by the GL renderer on frames returned by render_at.
// Those are clones — free them fully.
// ---------------------------------------------------------------------------
static void gfx_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) {
    sub_frame_unref(frame);
}

// ---------------------------------------------------------------------------
// gfx_resize
// ---------------------------------------------------------------------------
static int gfx_resize(SUB_FORMAT_BACKEND *be, int canvas_w, int canvas_h) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    // Canvas resize only (rotation, surface recreate) -- real_video_w/h and the video's
    // own box don't change just because the GL surface did; those come from
    // gfx_set_video_box() below, driven independently by SurfaceController.
    ctx->canvas_w = canvas_w;
    ctx->canvas_h = canvas_h;
    ctx->is_dirty = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_set_video_box
//
// Called whenever SurfaceController recomputes where the video itself sits
// on screen -- rotation, use_sub_margins toggling, a new video's aspect
// ratio changing the letterbox/pillarbox amount. Independent of gfx_resize:
// the canvas can resize without the video's box changing shape (e.g. a
// symmetric screen resize) and the box can change without the canvas
// resizing (e.g. margins preference flipped without rotating).
// ---------------------------------------------------------------------------
static int gfx_set_video_box(SUB_FORMAT_BACKEND *be, int x, int y, int w, int h) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    ctx->video_box_x = x;
    ctx->video_box_y = y;
    ctx->video_box_w = w;
    ctx->video_box_h = h;
    ctx->is_dirty = 1; // force a redraw with corrected geometry even if the bitmap itself is unchanged
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_flush
//
// Called on seek — clear the stored frame so stale bitmaps don't reappear.
// ---------------------------------------------------------------------------
static int gfx_flush(SUB_FORMAT_BACKEND *be) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (ctx->current_frame) {
        sub_frame_unref(ctx->current_frame);
        ctx->current_frame = NULL;
    }
    ctx->is_cleared = 1;
    ctx->is_dirty = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_close
// ---------------------------------------------------------------------------
static int gfx_close(SUB_FORMAT_BACKEND *be) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (ctx->current_frame) {
        sub_frame_unref(ctx->current_frame);
    }
    free(ctx);
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_get_timeout_ms
// ---------------------------------------------------------------------------
static int gfx_get_timeout_ms(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    return -1; // Infinite sleep. Only wakes when a feed/clear signals the engine.
}

// ---------------------------------------------------------------------------
// sub_format_gfx_create
// ---------------------------------------------------------------------------
SUB_FORMAT_BACKEND *sub_format_gfx_create(void) {
    SUB_FORMAT_BACKEND *be = calloc(1, sizeof(SUB_FORMAT_BACKEND));
    be->open        = gfx_open;
    be->feed_bitmap = gfx_feed_bitmap;
    be->render_at   = gfx_render_at;
    be->free_frame  = gfx_free_frame;
    be->resize      = gfx_resize;
    be->set_video_box = gfx_set_video_box;
    be->flush       = gfx_flush;
    be->close       = gfx_close;
    be->get_timeout_ms = gfx_get_timeout_ms;
    return be;
}
