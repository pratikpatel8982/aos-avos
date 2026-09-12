#include "sub_render_gl.h"
#include "sub_engine.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <pthread.h>

// Some NDK versions only define this in eglext.h under the *_KHR suffix
// rather than the EGL-1.5-core name; cover both so the build doesn't
// depend on exactly which headers ship in a given NDK release.
#ifndef EGL_OPENGL_ES3_BIT
#ifdef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT EGL_OPENGL_ES3_BIT_KHR
#else
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#endif
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include <time.h>

#define DBG if(Debug[DBG_SUB])

// --- ASS mask atlas + batched draw (Phase 2) ---
// R8 atlas texture: every ES3 device guarantees GL_MAX_TEXTURE_SIZE >= 2048,
// so this size never needs a capability query. 2048x2048x1 byte = 4MB CPU-side
// scratch, comfortably larger than a typical multi-layer ASS frame.
#define SUB_ATLAS_DIM             2048
// Safety cap, not an expected ceiling -- a frame with more mask images than
// this just flushes and starts a second batch (see draw_ass_batch()) rather
// than failing. Real ASS frames are nowhere near this even for karaoke.
#define SUB_BATCH_MAX_QUADS       256
#define SUB_BATCH_VERTS_PER_QUAD  6   // two triangles, no strip/primitive-restart bookkeeping
#define SUB_BATCH_FLOATS_PER_VERT 8   // x, y, u, v, r, g, b, a

struct SUB_RENDERER {
    ANativeWindow  *window;
    pthread_t       thread;
    pthread_mutex_t lock;
    int             running;
    int             surface_width;
    int             surface_height;
    int             ui_mode; // 0 = 2D, 1 = SBS, 2 = TB
    const SUB_FRAME *current_frame;
    int              pending_redraw; // unified "something changed, redraw regardless"
    uint64_t         applied_generation; // highest wakeup_generation this thread has finished a poll+store pass for
    uint64_t         frame_generation;   // bumped only when current_frame is swapped for genuinely new content -- see sub_render_gl_get_frame_generation()
    pthread_cond_t   frame_cond;         // broadcast whenever applied_generation advances
    GLuint           gl_program_rgba;  // straight textured quad -- GFX/PGS events (SUB_BITMAP_RGBA8)
    GLuint           gl_program_mask;  // single-image mask*color tint -- fallback only, see draw_mask_immediate()
    GLint            u_mask_color;     // "uColor" uniform location in gl_program_mask
    GLuint           gl_program_batch; // batched mask*color tint -- the common-case ASS path, color rides per-vertex
    GLuint           gl_texture;       // shared by gl_program_rgba draws and the gl_program_mask fallback
    GLuint           gl_atlas_texture; // persistent R8 atlas for gl_program_batch
    GLuint           gl_vao;           // persistent vertex layout for the batched draw (pos/uv/color)
    GLuint           gl_vbo;           // persistent vertex buffer backing that VAO
    uint8_t         *atlas_cpu;        // SUB_ATLAS_DIM*SUB_ATLAS_DIM scratch, packed fresh each redraw, reused across redraws
    GLfloat         *batch_vertices;   // SUB_BATCH_MAX_QUADS*SUB_BATCH_VERTS_PER_QUAD*SUB_BATCH_FLOATS_PER_VERT scratch
    GLint            attrib_pos;       // shared across gl_program_rgba/gl_program_mask -- explicit
    GLint            attrib_tex;       // glBindAttribLocation(0/1) in create_program() guarantees this
    void            *engine;
};

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buf[512];
        glGetShaderInfoLog(shader, sizeof(buf), NULL, buf);
        serprintf("Shader compile error: %s\n", buf);
    }
    return shader;
}

static GLuint create_program(const char *vertex_src, const char *fragment_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    // Pin all three programs to the same attribute slots so one set of
    // locations (queried once, below) is valid no matter which program is
    // currently bound when we draw. gl_program_rgba/gl_program_mask don't
    // declare "aColor" -- binding it anyway is a documented no-op per the
    // GL spec (a binding only takes effect for attributes the shader
    // actually uses), so this is safe to call unconditionally.
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    glBindAttribLocation(program, 2, "aColor");
    glLinkProgram(program);

    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char buf[512];
        glGetProgramInfoLog(program, sizeof(buf), NULL, buf);
        serprintf("Program link error: %s\n", buf);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// --- ASS mask fallback: single oversized image ---
// Only reached when one mask image doesn't fit in the atlas at all (w or h
// > SUB_ATLAS_DIM -- effectively never for real subtitle content, since
// images are bounded by video_w/video_h and ES3's guaranteed minimum
// texture size is 2048). Draws it immediately with its own texture upload,
// same as every mask image did before batching existed. Exists so an
// oversized image degrades to "one extra draw call" instead of being
// silently dropped.
static void draw_mask_immediate(SUB_RENDERER *r, const SUB_FRAME *frame_to_draw, SUB_EVENT *ev) {
    glBindVertexArray(0); // don't fight the batch VAO's bound attribute state
    glBindTexture(GL_TEXTURE_2D, r->gl_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ev->w, ev->h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, ev->data.bitmap.pixels);
    glUseProgram(r->gl_program_mask);
    SUB_COLOR c = ev->data.bitmap.color;
    glUniform4f(r->u_mask_color, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);

    float x1 = (ev->x / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
    float y1 = 1.0f - (ev->y / (float)frame_to_draw->video_h) * 2.0f;
    float x2 = ((ev->x + ev->w) / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
    float y2 = 1.0f - ((ev->y + ev->h) / (float)frame_to_draw->video_h) * 2.0f;

    GLfloat vertices[] = {
        x1, y2, 0.0f, 0.0f, 1.0f,
        x2, y2, 0.0f, 1.0f, 1.0f,
        x1, y1, 0.0f, 0.0f, 0.0f,
        x2, y1, 0.0f, 1.0f, 0.0f
    };
    glVertexAttribPointer(r->attrib_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(r->attrib_tex, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices + 3);
    glEnableVertexAttribArray(r->attrib_pos);
    glEnableVertexAttribArray(r->attrib_tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(r->attrib_pos);
    glDisableVertexAttribArray(r->attrib_tex);
}

// Uploads whatever's been packed into atlas_cpu so far and draws all of it
// in one call. The only glTexImage2D + glDrawArrays pair for however many
// mask images fit in this batch, instead of one pair per image.
static void flush_ass_batch(SUB_RENDERER *r, int quad_count) {
    if (quad_count <= 0) return;

    glBindTexture(GL_TEXTURE_2D, r->gl_atlas_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SUB_ATLAS_DIM, SUB_ATLAS_DIM, 0,
                 GL_RED, GL_UNSIGNED_BYTE, r->atlas_cpu);

    glUseProgram(r->gl_program_batch);
    glBindVertexArray(r->gl_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                     (GLsizeiptr)quad_count * SUB_BATCH_VERTS_PER_QUAD * SUB_BATCH_FLOATS_PER_VERT * sizeof(GLfloat),
                     r->batch_vertices);

    glDrawArrays(GL_TRIANGLES, 0, quad_count * SUB_BATCH_VERTS_PER_QUAD);

    glBindVertexArray(0);
}

// Packs every SUB_BITMAP_MASK_R8 event in the frame's event list into the
// atlas with a simple shelf packer, building one vertex per corner (with
// this image's own color baked into those vertices -- see
// SUB_BATCH_FLOATS_PER_VERT) as it goes, and flushes in as few draw calls
// as the atlas/batch size allows. RGBA8 (GFX) events are left untouched for
// the caller's existing immediate-draw loop.
static void draw_ass_batch(SUB_RENDERER *r, const SUB_FRAME *frame_to_draw) {
    if (!r->atlas_cpu || !r->batch_vertices) return; // allocation failed at init -- nothing to batch with

    int cursor_x = 0, cursor_y = 0, shelf_h = 0;
    int quad_count = 0;
    GLfloat *vtx = r->batch_vertices;

    for (SUB_EVENT *ev = frame_to_draw->events; ev; ev = ev->next) {
        if (ev->kind != SUB_EVENT_BITMAP || ev->data.bitmap.format != SUB_BITMAP_MASK_R8)
            continue;

        int iw = ev->w, ih = ev->h;
        if (iw <= 0 || ih <= 0) continue;

        if (iw > SUB_ATLAS_DIM || ih > SUB_ATLAS_DIM) {
            // libass's image list is already in back-to-front paint order
            // (outline under fill, etc). Flush whatever's queued first so
            // this image can't jump ahead of quads that preceded it in the
            // list -- otherwise drawing it immediately here would reorder
            // it relative to anything still sitting in the batch buffer.
            flush_ass_batch(r, quad_count);
            cursor_x = 0; cursor_y = 0; shelf_h = 0; quad_count = 0;
            vtx = r->batch_vertices;

            draw_mask_immediate(r, frame_to_draw, ev);
            continue;
        }

        if (cursor_x + iw > SUB_ATLAS_DIM) {
            cursor_x = 0;
            cursor_y += shelf_h;
            shelf_h = 0;
        }
        if (cursor_y + ih > SUB_ATLAS_DIM || quad_count >= SUB_BATCH_MAX_QUADS) {
            // Out of room (or hit the per-draw cap) -- flush what we have
            // and start a fresh atlas/batch for the remaining images. The
            // image that triggered this is guaranteed to fit an empty
            // atlas (already checked against SUB_ATLAS_DIM above).
            flush_ass_batch(r, quad_count);
            cursor_x = 0; cursor_y = 0; shelf_h = 0; quad_count = 0;
            vtx = r->batch_vertices;
        }

        const uint8_t *src = ev->data.bitmap.pixels;
        for (int y = 0; y < ih; y++) {
            memcpy(r->atlas_cpu + (size_t)(cursor_y + y) * SUB_ATLAS_DIM + cursor_x,
                   src + (size_t)y * ev->data.bitmap.stride, iw);
        }

        float x1 = (ev->x / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
        float y1 = 1.0f - (ev->y / (float)frame_to_draw->video_h) * 2.0f;
        float x2 = ((ev->x + iw) / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
        float y2 = 1.0f - ((ev->y + ih) / (float)frame_to_draw->video_h) * 2.0f;

        float u1 = cursor_x / (float)SUB_ATLAS_DIM;
        float v1 = (cursor_y + ih) / (float)SUB_ATLAS_DIM;
        float u2 = (cursor_x + iw) / (float)SUB_ATLAS_DIM;
        float v2 = cursor_y / (float)SUB_ATLAS_DIM;

        SUB_COLOR c = ev->data.bitmap.color;
        float cr = c.r / 255.0f, cg = c.g / 255.0f, cb = c.b / 255.0f, ca = c.a / 255.0f;

        // Two triangles per quad: (x1,y2)-(x2,y2)-(x1,y1) and (x2,y2)-(x2,y1)-(x1,y1)
        float quad[SUB_BATCH_VERTS_PER_QUAD][4] = {
            { x1, y2, u1, v1 }, { x2, y2, u2, v1 }, { x1, y1, u1, v2 },
            { x2, y2, u2, v1 }, { x2, y1, u2, v2 }, { x1, y1, u1, v2 },
        };
        for (int i = 0; i < SUB_BATCH_VERTS_PER_QUAD; i++) {
            *vtx++ = quad[i][0]; *vtx++ = quad[i][1]; *vtx++ = quad[i][2]; *vtx++ = quad[i][3];
            *vtx++ = cr; *vtx++ = cg; *vtx++ = cb; *vtx++ = ca;
        }

        cursor_x += iw;
        shelf_h = shelf_h > ih ? shelf_h : ih;
        quad_count++;
    }

    flush_ass_batch(r, quad_count);
}

static void* egl_render_thread(void* arg) {
    SUB_RENDERER *r = (SUB_RENDERER*)arg;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs;
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);

    EGLSurface surface = EGL_NO_SURFACE;
    ANativeWindow *current_window = NULL;

    while (1) {
        // 1. Sample the generation counter BEFORE doing any work
        uint64_t loop_generation = 0;
        if (r->engine) {
            loop_generation = sub_engine_get_generation((SUB_ENGINE*)r->engine);
        }

        int needs_redraw = 0;

        pthread_mutex_lock(&r->lock);
        if (!r->running) {
            pthread_mutex_unlock(&r->lock);
            break;
        }

        // 2. Safely acquire our own strong reference to the window
        ANativeWindow *target_window = r->window;
        if (target_window) {
            ANativeWindow_acquire(target_window);
        }
        pthread_mutex_unlock(&r->lock);

        // --- CLEAN EGL SURFACE CREATION ---
        if (target_window != current_window) {
            if (surface != EGL_NO_SURFACE) {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(display, surface);
                surface = EGL_NO_SURFACE;
                pthread_mutex_lock(&r->lock);
                r->surface_width  = 0;
                r->surface_height = 0;
                pthread_mutex_unlock(&r->lock);
            }

            if (current_window) ANativeWindow_release(current_window);
            current_window = target_window; // Transfer ownership

            current_window = target_window;

            if (current_window) {
                surface = eglCreateWindowSurface(display, config, current_window, NULL);
                needs_redraw = 1; // <--- 2. FORCE REDRAW ON NEW SURFACE
                if (surface != EGL_NO_SURFACE) {
                    eglMakeCurrent(display, surface, surface, context);
                    eglSwapInterval(display, 1);

                    // Adopt this new surface's REAL pixel size right away, instead of
                    // waiting for a separate sub_render_gl_resize() call to arrive from
                    // the Java/JNI side. That call is driven by an independent callback
                    // (TextureView's onSurfaceTextureSizeChanged) whose timing relative
                    // to attach_surface() isn't guaranteed -- e.g. switching to the
                    // floating player's window here would otherwise keep drawing with
                    // r->surface_width/height still left over from whichever window
                    // (typically full-screen) was attached before, until that callback
                    // happens to land. Querying EGL directly makes this self-correcting.
                    EGLint real_w = 0, real_h = 0;
                    eglQuerySurface(display, surface, EGL_WIDTH, &real_w);
                    eglQuerySurface(display, surface, EGL_HEIGHT, &real_h);
                    if (real_w > 0 && real_h > 0) {
                        pthread_mutex_lock(&r->lock);
                        r->surface_width  = real_w;
                        r->surface_height = real_h;
                        pthread_mutex_unlock(&r->lock);
                        DBG serprintf("SUB_RENDER_GL: adopted real EGL surface size %d x %d on window attach\n", real_w, real_h);
                    }

                    if (r->gl_program_rgba == 0) {
                        const char* vs_src =
                        "#version 300 es\n"
                        "in vec4 aPosition;\n"
                        "in vec2 aTexCoord;\n"
                        "out vec2 vTexCoord;\n"
                        "void main() {\n"
                        "  gl_Position = aPosition;\n"
                        "  vTexCoord = aTexCoord;\n"
                        "}\n";

        // GFX/PGS path: texture already holds real RGBA color, just sample it.
        const char* fs_src_rgba =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  fragColor = texture(uTexture, vTexCoord);\n"
        "}\n";

        // ASS path: texture holds an 8-bit coverage mask in the red channel;
        // tint it with this image's color instead of the CPU pre-expanding
        // every pixel to RGBA before it ever reaches the GPU.
        const char* fs_src_mask =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec4 uColor;\n" // rgb + alpha, all already normalized 0..1
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  float coverage = texture(uTexture, vTexCoord).r;\n"
        "  fragColor = vec4(uColor.rgb, coverage * uColor.a);\n"
        "}\n";

        r->gl_program_rgba = create_program(vs_src, fs_src_rgba);
        r->gl_program_mask = create_program(vs_src, fs_src_mask);
        // Both programs bind aPosition/aTexCoord to locations 0/1 (see
        // create_program()), so querying either program gives locations
        // valid for both -- no need to look these up per-program.
        r->attrib_pos  = glGetAttribLocation(r->gl_program_rgba, "aPosition");
        r->attrib_tex  = glGetAttribLocation(r->gl_program_rgba, "aTexCoord");
        r->u_mask_color = glGetUniformLocation(r->gl_program_mask, "uColor");

        // Batched ASS path: color rides per-vertex (aColor) instead of a
        // uniform, so one draw call can mix images that each have their
        // own color (e.g. fill vs. outline vs. shadow layers) -- a single
        // "uColor" uniform couldn't do that across a batch.
        const char* vs_src_batch =
        "#version 300 es\n"
        "in vec2 aPosition;\n"
        "in vec2 aTexCoord;\n"
        "in vec4 aColor;\n"
        "out vec2 vTexCoord;\n"
        "out vec4 vColor;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "  vTexCoord = aTexCoord;\n"
        "  vColor = aColor;\n"
        "}\n";
        const char* fs_src_batch =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vTexCoord;\n"
        "in vec4 vColor;\n"
        "uniform sampler2D uTexture;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  float coverage = texture(uTexture, vTexCoord).r;\n"
        "  fragColor = vec4(vColor.rgb, coverage * vColor.a);\n"
        "}\n";
        r->gl_program_batch = create_program(vs_src_batch, fs_src_batch);

        glGenTextures(1, &r->gl_texture);
        glBindTexture(GL_TEXTURE_2D, r->gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Set once here rather than every event in the draw loop below --
        // it never needs to be anything else for these tightly-packed
        // uploads (see the loop for why per-event was wasted work).
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // Persistent R8 atlas texture for the batched ASS path.
        glGenTextures(1, &r->gl_atlas_texture);
        glBindTexture(GL_TEXTURE_2D, r->gl_atlas_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Persistent VBO + VAO: vertex layout (pos.xy, uv.xy, color.rgba)
        // is configured exactly once here, not re-enabled/disabled on
        // every draw the way the immediate paths above still do.
        glGenBuffers(1, &r->gl_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, r->gl_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)SUB_BATCH_MAX_QUADS * SUB_BATCH_VERTS_PER_QUAD * SUB_BATCH_FLOATS_PER_VERT * sizeof(GLfloat),
                     NULL, GL_DYNAMIC_DRAW);

        glGenVertexArrays(1, &r->gl_vao);
        glBindVertexArray(r->gl_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->gl_vbo);
        GLsizei batch_stride = SUB_BATCH_FLOATS_PER_VERT * sizeof(GLfloat);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, batch_stride, (const void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, batch_stride, (const void*)(2 * sizeof(GLfloat)));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, batch_stride, (const void*)(4 * sizeof(GLfloat)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);

        r->atlas_cpu = calloc(1, (size_t)SUB_ATLAS_DIM * SUB_ATLAS_DIM);
        r->batch_vertices = malloc((size_t)SUB_BATCH_MAX_QUADS * SUB_BATCH_VERTS_PER_QUAD * SUB_BATCH_FLOATS_PER_VERT * sizeof(GLfloat));
                    }
                }
            }
        } else {
            // Unchanged, release the temporary check reference
            if (target_window) ANativeWindow_release(target_window);
        }

        // --- HYBRID FIX: ALWAYS POLL THE ENGINE ---
        // Even if the 3D Mode deactivated the GPU Surface, we MUST continue
        // to poll the clock so the memory frames update for the CPU Blender!
        SUB_FRAME *new_frame = NULL;
        if (r->engine) {
            new_frame = sub_engine_poll_frame((SUB_ENGINE*)r->engine);
        }

        pthread_mutex_lock(&r->lock);
        if (r->pending_redraw) {
            r->pending_redraw = 0;
            needs_redraw = 1;
        }
        if (new_frame != NULL && new_frame != r->current_frame) {
            if (r->current_frame) {
                sub_engine_release_frame((SUB_ENGINE*)r->engine, (SUB_FRAME*)r->current_frame);
            }
            r->current_frame = new_frame;
            r->frame_generation++; // content genuinely changed -- see field doc in the struct
            needs_redraw = 1; // <--- 3. FORCE REDRAW ON NEW FRAME
        }

        // This iteration's poll_frame() (above) has now run and r->current_frame reflects
        // its result, so anything that was true when loop_generation was sampled at the top
        // of this iteration -- e.g. a style change whose force_wake() bump was already
        // visible at that point -- is guaranteed to be reflected here. Stamp and broadcast so
        // sub_render_gl_wait_for_generation() callers (the 3D pull path) can unblock.
        r->applied_generation = loop_generation;
        pthread_cond_broadcast(&r->frame_cond);

        int w = r->surface_width;
        int h = r->surface_height;
        const SUB_FRAME *frame_to_draw = r->current_frame;
        GLint attrib_pos = r->attrib_pos;
        GLint attrib_tex = r->attrib_tex;

        // We're about to use frame_to_draw's pixel data (glTexImage2D) after
        // unlocking below, in the branch where we actually draw. Take our own
        // reference to it now, under the lock, so another thread calling
        // sub_render_gl_clear()/close_track()/destroy() concurrently can't drop
        // this exact frame's refcount to zero and free() its rgba buffer out
        // from under us mid-upload. Matched by sub_engine_release_frame() right
        // after eglSwapBuffers() below, or immediately if we end up not drawing.
        int will_draw = (surface != EGL_NO_SURFACE) && needs_redraw;
        if (frame_to_draw && will_draw) {
            sub_frame_ref((SUB_FRAME *)frame_to_draw);
        }
        pthread_mutex_unlock(&r->lock);

        // If EGL is offline (e.g. we are in 3D Canvas mode), sleep and skip drawing.
        // The Java onFrameAvailable() callback will extract frames via RAM instead.
        if (surface == EGL_NO_SURFACE) {
            // will_draw was false in this branch, so no ref was taken -- nothing to release.
            if (r->engine) {
                sub_engine_wait_event((SUB_ENGINE*)r->engine, loop_generation);
            } else {
                usleep(16000); // Fallback if engine isn't attached yet
            }
            continue;
        }

        // --- 4. THE GL BYPASS ---
        // If the surface didn't change and the frame didn't change, skip the GPU!
        if (!needs_redraw) {
            // will_draw was false in this branch too -- no ref was taken.
            if (r->engine) {
                // Pass the generation counter so we don't drop wakes
                sub_engine_wait_event((SUB_ENGINE*)r->engine, loop_generation);
            } else {
                usleep(16000); // Fallback if engine isn't attached yet
            }
            continue;
        }

        // --- NATIVE 2D OPENGL RENDERING ---
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (frame_to_draw && frame_to_draw->events
            && r->gl_program_rgba != 0 && r->gl_program_mask != 0 && r->gl_program_batch != 0
            && attrib_pos != -1 && attrib_tex != -1) {

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // ASS mask events: packed into the atlas and drawn in as few calls
        // as possible (see draw_ass_batch()). GFX/PGS events are left for
        // the loop below -- a frame never carries more than one of those
        // (see sub_format_gfx.c), so there's nothing there worth batching.
        draw_ass_batch(r, frame_to_draw);
        glBindVertexArray(0); // draw_ass_batch leaves its VAO bound after a flush

        SUB_EVENT *ev = frame_to_draw->events;
        while (ev) {
            if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.format == SUB_BITMAP_RGBA8) {
                glBindTexture(GL_TEXTURE_2D, r->gl_texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                             ev->w, ev->h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE,
                             ev->data.bitmap.pixels);
                glUseProgram(r->gl_program_rgba);

                float x1 = (ev->x / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                float y1 = 1.0f - (ev->y / (float)frame_to_draw->video_h) * 2.0f;
                float x2 = ((ev->x + ev->w) / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                float y2 = 1.0f - ((ev->y + ev->h) / (float)frame_to_draw->video_h) * 2.0f;

                GLfloat vertices[] = {
                    x1, y2, 0.0f,  0.0f, 1.0f,
                    x2, y2, 0.0f,  1.0f, 1.0f,
                    x1, y1, 0.0f,  0.0f, 0.0f,
                    x2, y1, 0.0f,  1.0f, 0.0f
                };

                glVertexAttribPointer(attrib_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices);
                glVertexAttribPointer(attrib_tex, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices + 3);

                glEnableVertexAttribArray(attrib_pos);
                glEnableVertexAttribArray(attrib_tex);

                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                glDisableVertexAttribArray(attrib_pos);
                glDisableVertexAttribArray(attrib_tex);
            }
            ev = ev->next;
        }
        glDisable(GL_BLEND);
            }

            eglSwapBuffers(display, surface);

        // Done reading frame_to_draw's pixels -- release the pin taken above.
        if (frame_to_draw && will_draw) {
            sub_engine_release_frame((SUB_ENGINE*)r->engine, (SUB_FRAME*)frame_to_draw);
        }

    }

    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (current_window) {
        ANativeWindow_release(current_window);   // NEW
    }
    if (r->gl_program_rgba != 0) {
        glDeleteProgram(r->gl_program_rgba);
        glDeleteProgram(r->gl_program_mask);
        glDeleteProgram(r->gl_program_batch);
        glDeleteTextures(1, &r->gl_texture);
        glDeleteTextures(1, &r->gl_atlas_texture);
        glDeleteVertexArrays(1, &r->gl_vao);
        glDeleteBuffers(1, &r->gl_vbo);
        free(r->atlas_cpu);
        free(r->batch_vertices);
        r->atlas_cpu = NULL;
        r->batch_vertices = NULL;
    }
    eglDestroyContext(display, context);
    eglTerminate(display);
    return NULL;
}

SUB_RENDERER *sub_render_gl_create(void *engine) {
    SUB_RENDERER *r = calloc(1, sizeof(SUB_RENDERER));
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->frame_cond, NULL);
    r->running    = 1;
    r->attrib_pos = -1;
    r->attrib_tex = -1;
    // Assign BEFORE pthread_create(): the render thread's loop reads
    // r->engine without the lock (it's only ever unlocked-read there, and
    // this is the only write, so this ordering is what actually makes that
    // safe). Passing it in up front means the thread's first iteration
    // already observes the real pointer instead of racing a later write.
    r->engine = engine;
    pthread_create(&r->thread, NULL, egl_render_thread, r);
    return r;
}

void sub_render_gl_destroy(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->running = 0;
    // Grab and null any frame that poll_frame() may have installed between
    // the last sub_render_gl_clear() call and now. The render thread is still
    // running at this point (pthread_join not called yet), so we must hold
    // the lock while stealing the pointer. After join the thread is dead and
    // cannot install a new frame, so no further lock is needed for the free.
    SUB_FRAME *leftover = (SUB_FRAME *)r->current_frame;
    r->current_frame = NULL;
    pthread_mutex_unlock(&r->lock);
    // WAKE THE THREAD SO IT CAN EXIT!
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine);
    }
    pthread_join(r->thread, NULL);
    // The engine is already destroyed by this point (sub_engine_destroy calls
    // close_track then destroy_renderer), so use the bare global free —
    // no backend vtable to route through.
    if (leftover) sub_engine_free_frame(leftover);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->frame_cond);
    free(r);
}

void sub_render_gl_attach_surface(SUB_RENDERER *r, ANativeWindow *window) {
    pthread_mutex_lock(&r->lock);
    if (r->window) {
        ANativeWindow_release(r->window);
    }
    r->window = window;
    if (r->window) {
        ANativeWindow_acquire(r->window);
    }
    pthread_mutex_unlock(&r->lock);
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine);   // NEW
    }
}

void sub_render_gl_detach_surface(SUB_RENDERER *r) {
    sub_render_gl_attach_surface(r, NULL);
}

void sub_render_gl_resize(SUB_RENDERER *r, int width, int height) {
    pthread_mutex_lock(&r->lock);
    r->surface_width  = width;
    r->surface_height = height;
    // Force the native buffer to this size synchronously. Without this,
    // glViewport() below assumes a buffer size that SurfaceFlinger hasn't
    // necessarily allocated yet -- buffer resize isn't guaranteed to be
    // synchronous with this JNI call, so subs briefly draw stretched into
    // the old buffer until a later redraw happens to land after it catches
    // up.
    if (r->window) {
        ANativeWindow_setBuffersGeometry(r->window, width, height, 0);
    }
    pthread_mutex_unlock(&r->lock);
    sub_render_gl_invalidate_cache(r);
}

void sub_render_gl_set_ui_mode(SUB_RENDERER *r, int mode) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->ui_mode = mode;
    pthread_mutex_unlock(&r->lock);
}

uint64_t sub_render_gl_get_frame_generation(SUB_RENDERER *r) {
    if (!r) return 0;
    pthread_mutex_lock(&r->lock);
    uint64_t gen = r->frame_generation;
    pthread_mutex_unlock(&r->lock);
    return gen;
}

void sub_render_gl_clear(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    SUB_FRAME *to_free = (SUB_FRAME*)r->current_frame;
    r->current_frame = NULL;
    pthread_mutex_unlock(&r->lock);

    if (to_free) {
        sub_engine_release_frame((SUB_ENGINE*)r->engine, to_free);
    }
    sub_render_gl_invalidate_cache(r);
}

void sub_render_gl_invalidate_cache(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->pending_redraw = 1;
    pthread_mutex_unlock(&r->lock);
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine); // wake it if it's parked
    }
}

// Blocks the calling thread -- typically a JNI call arriving on the Java UI thread --
// until the render thread has completed a poll+store pass stamped with a
// loop_generation >= target_generation, or until timeout_ms elapses, whichever comes
// first. This is what closes the race in the 3D hybrid pull path: force_wake() only
// guarantees the render thread will eventually notice a style change, not that it
// already has by the time fill_bitmap() runs on another thread. Never blocks
// indefinitely -- a wedged or slow render thread just means the caller falls through
// and draws whatever's currently cached, rather than hanging the UI thread.
void sub_render_gl_wait_for_generation(SUB_RENDERER *r, uint64_t target_generation, int timeout_ms) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    if (r->applied_generation >= target_generation) {
        pthread_mutex_unlock(&r->lock);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long nsec = ts.tv_nsec + ((long long)timeout_ms * 1000000LL);
    ts.tv_sec += nsec / 1000000000LL;
    ts.tv_nsec = nsec % 1000000000LL;
    while (r->applied_generation < target_generation) {
        if (pthread_cond_timedwait(&r->frame_cond, &r->lock, &ts) != 0) {
            break; // timed out (or spurious wake past deadline) -- bail, don't hang the caller
        }
    }
    pthread_mutex_unlock(&r->lock);
}

// Shared "src-over" blend, used by both format branches in
// sub_render_gl_fill_bitmap() below so the math only lives in one place.
static inline void cpu_blend_over(uint8_t *dst_px, uint8_t r, uint8_t g, uint8_t b, uint8_t sa) {
    if (sa == 255 || dst_px[3] == 0) {
        dst_px[0] = r; dst_px[1] = g; dst_px[2] = b; dst_px[3] = sa;
    } else {
        uint8_t dr = dst_px[0], dg = dst_px[1], db = dst_px[2], da = dst_px[3];
        int inv_sa = 255 - sa;
        dst_px[0] = (r * sa + dr * inv_sa) >> 8;
        dst_px[1] = (g * sa + dg * inv_sa) >> 8;
        dst_px[2] = (b * sa + db * inv_sa) >> 8;
        dst_px[3] = sa + ((da * inv_sa) >> 8);
    }
}

// --- HYBRID 3D BRIDGE FAST CPU BLENDER ---
int sub_render_gl_fill_bitmap(SUB_RENDERER *r, void* pixels, int dst_w, int dst_h, int dst_stride, uint64_t *out_generation) {
    if (!r) return 0;
    int has_subs = 0;

    pthread_mutex_lock(&r->lock);
    const SUB_FRAME *frame = r->current_frame;
    // Stamped in the same critical section as the blend below, so a concurrent frame
    // swap (egl_render_thread bumping frame_generation and replacing current_frame)
    // can't land between "we blended frame X" and "we reported X's generation" --
    // the caller always gets the generation of the exact frame it just read.
    if (out_generation) *out_generation = r->frame_generation;

    if (frame && frame->events) {
        has_subs = 1;
        SUB_EVENT *ev = frame->events;

        while (ev) {
            if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.pixels) {
                int src_w = ev->w;
                int src_h = ev->h;
                int src_x = ev->x;
                int src_y = ev->y;

                if (ev->data.bitmap.format == SUB_BITMAP_MASK_R8) {
                    // ASS: 8-bit coverage mask + one color for the whole image.
                    // Tint here instead of assuming the buffer is RGBA -- this
                    // is the CPU-side counterpart of the GL fragment shader's
                    // "coverage * uColor" in the draw loop above.
                    const uint8_t *src_mask = ev->data.bitmap.pixels;
                    SUB_COLOR c = ev->data.bitmap.color;

                    if (c.a != 0) {
                        for (int y = 0; y < src_h; y++) {
                            int dy = src_y + y;
                            if (dy < 0 || dy >= dst_h) continue;

                            uint8_t *dst_row = (uint8_t *)pixels + (dy * dst_stride);
                            const uint8_t *src_row = src_mask + (y * ev->data.bitmap.stride);

                            for (int x = 0; x < src_w; x++) {
                                int dx = src_x + x;
                                if (dx < 0 || dx >= dst_w) continue;

                                uint8_t mask = src_row[x];
                                if (mask == 0) continue;
                                uint8_t sa = (uint8_t)((mask * c.a) / 255);
                                if (sa == 0) continue;

                                cpu_blend_over(dst_row + (dx * 4), c.r, c.g, c.b, sa);
                            }
                        }
                    }
                } else {
                    // GFX/PGS: already real RGBA, 1:1 pixel copy. No scaling,
                    // no rounding errors, no clipping!
                    const uint8_t *src_rgba = ev->data.bitmap.pixels;

                    for (int y = 0; y < src_h; y++) {
                        int dy = src_y + y;
                        if (dy < 0 || dy >= dst_h) continue;

                        uint8_t *dst_row = (uint8_t *)pixels + (dy * dst_stride);
                        const uint8_t *src_row = src_rgba + (y * ev->data.bitmap.stride);

                        for (int x = 0; x < src_w; x++) {
                            int dx = src_x + x;
                            if (dx < 0 || dx >= dst_w) continue;

                            uint8_t *dst_px = dst_row + (dx * 4);
                            const uint8_t *src_px = src_row + (x * 4);

                            uint8_t sa = src_px[3];
                            if (sa == 0) continue;

                            cpu_blend_over(dst_px, src_px[0], src_px[1], src_px[2], sa);
                        }
                    }
                }
            }
            ev = ev->next;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return has_subs;
}
