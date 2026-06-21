#include "sub_format.h"
#include <ass/ass.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <android/log.h>

#define LOG_TAG "SubFormatSSA"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

#define LIBASS_DEFAULT_FONT   "/system/fonts/Roboto-Regular.ttf"
#define LIBASS_DEFAULT_FAMILY "sans-serif"

typedef struct {
    ASS_Library    *library;
    ASS_Renderer   *renderer;
    ASS_Track      *track;
    pthread_mutex_t lock;
    int             video_w, video_h;
} SSA_BACKEND;

static void ass_msg_cb(int level, const char *fmt, va_list va, void *data) {
    if (level < 4) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        LOGD("LIBASS[%d]: %s", level, buf);
    }
}

static int ssa_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    SSA_BACKEND *ctx = calloc(1, sizeof(SSA_BACKEND));
    pthread_mutex_init(&ctx->lock, NULL);

    ctx->video_w = params->video_w;
    ctx->video_h = params->video_h;

    ctx->library = ass_library_init();
    ass_set_message_cb(ctx->library, ass_msg_cb, NULL);
    ass_set_extract_fonts(ctx->library, 1);

    ctx->renderer = ass_renderer_init(ctx->library);
    ass_set_frame_size(ctx->renderer, ctx->video_w > 0 ? ctx->video_w : 1920,
                       ctx->video_h > 0 ? ctx->video_h : 1080);

    ctx->track = ass_new_track(ctx->library);

    if (params->codec_private && params->codec_private_size > 0) {
        ass_process_codec_private(ctx->track, (char *)params->codec_private, params->codec_private_size);
    }
    const char *font_path = (params->fallback_font_path && params->fallback_font_path[0])
                ? params->fallback_font_path : LIBASS_DEFAULT_FONT;

    ass_set_fonts(ctx->renderer, font_path, LIBASS_DEFAULT_FAMILY, ASS_FONTPROVIDER_NONE, NULL, 1);
    LOGD("ssa_open: using fallback font: %s", font_path);

    be->priv = ctx;
    return 0;
}

static int ssa_feed(SUB_FORMAT_BACKEND *be, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;

    // NO MORE 4-BYTE STRIPPING HERE. The data pointer is pure string.
    pthread_mutex_lock(&ctx->lock);
    if (size >= 10 && strncmp((const char*)data, "Dialogue: ", 10) == 0) {
            ass_process_data(ctx->track, (char *)data, size);
        } else {
            int64_t final_dur = duration_ms > 0 ? duration_ms : 0;
            ass_process_chunk(ctx->track, (char *)data, size, pts_ms, final_dur);
        }
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static SUB_FRAME *ssa_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);

    int change = 0;
    ASS_Image *imgs = ass_render_frame(ctx->renderer, ctx->track, pts_ms, &change);

    if (!change && imgs != NULL) {
        pthread_mutex_unlock(&ctx->lock);
        return NULL;
    }

    SUB_FRAME *frame = calloc(1, sizeof(SUB_FRAME));
    frame->pts_ms = pts_ms;
    frame->video_w = ctx->video_w > 0 ? ctx->video_w : 1920;
    frame->video_h = ctx->video_h > 0 ? ctx->video_h : 1080;

    SUB_EVENT *last_ev = NULL;

    for (ASS_Image *img = imgs; img; img = img->next) {
        if (!img->w || !img->h) continue;
        uint8_t a = 255 - (img->color & 0xFF);
        if (a == 0) continue;

        uint8_t r = (img->color >> 24) & 0xFF;
        uint8_t g = (img->color >> 16) & 0xFF;
        uint8_t b = (img->color >>  8) & 0xFF;

        SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
        ev->kind = SUB_EVENT_BITMAP;
        ev->x = img->dst_x;
        ev->y = img->dst_y;
        ev->w = img->w;
        ev->h = img->h;

        uint8_t *rgba = malloc(img->w * img->h * 4);
        const uint8_t *src = img->bitmap;
        uint8_t *dst = rgba;

        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                uint8_t mask = src[x];
                uint8_t final_a = (mask * a) / 255;

                dst[0] = r;
                dst[1] = g;
                dst[2] = b;
                dst[3] = final_a;
                dst += 4;
            }
            src += img->stride;
        }

        ev->data.bitmap.rgba = rgba;
        ev->data.bitmap.stride = img->w * 4;

        if (!frame->events) frame->events = ev;
        else last_ev->next = ev;
        last_ev = ev;
    }

    pthread_mutex_unlock(&ctx->lock);
    return frame;
}

static void ssa_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) {
    if (!frame) return;
    SUB_EVENT *ev = frame->events;
    while (ev) {
        SUB_EVENT *next = ev->next;
        if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
            free((void*)ev->data.bitmap.rgba);
        }
        free(ev);
        ev = next;
    }
    free(frame);
}

static int ssa_resize(SUB_FORMAT_BACKEND *be, int video_w, int video_h) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    ctx->video_w = video_w;
    ctx->video_h = video_h;
    ass_set_frame_size(ctx->renderer, video_w, video_h);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int ssa_flush(SUB_FORMAT_BACKEND *be) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    ass_flush_events(ctx->track);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int ssa_close(SUB_FORMAT_BACKEND *be) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->track) ass_free_track(ctx->track);
    if (ctx->renderer) ass_renderer_done(ctx->renderer);
    if (ctx->library) ass_library_done(ctx->library);
    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return 0;
}

SUB_FORMAT_BACKEND *sub_format_ssa_create(void) {
    SUB_FORMAT_BACKEND *be = calloc(1, sizeof(SUB_FORMAT_BACKEND));
    be->open = ssa_open;
    be->feed = ssa_feed;
    be->render_at = ssa_render_at;
    be->free_frame = ssa_free_frame;
    be->resize = ssa_resize;
    be->flush = ssa_flush;
    be->close = ssa_close;
    return be;
}
