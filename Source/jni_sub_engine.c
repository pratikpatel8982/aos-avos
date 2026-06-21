#include "jni_sub_engine.h"
#include "sub_engine.h"
#include "sub_style.h"
#include <android/native_window_jni.h>
#include <stddef.h>

// NEW: Global pointer so the AVOS core demuxer can easily find the engine
SUB_ENGINE *g_sub_engine = NULL;

// Helper to extract the engine pointer
static SUB_ENGINE* get_engine(jlong handle) {
    return (SUB_ENGINE*)(intptr_t)handle;
}

JNIEXPORT jlong JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz, jstring fallbackFontPath) {
    const char *font_path = NULL;
    if (fallbackFontPath != NULL) {
        font_path = (*env)->GetStringUTFChars(env, fallbackFontPath, NULL);
    }

    /* sub_engine_create() strdup()s this internally, so it's safe to
    * release Java's buffer immediately after the call returns —
    * SUB_ENGINE does not hold onto the pointer we pass here. */
    SUB_ENGINE *eng = sub_engine_create(font_path);
    if (font_path != NULL) {
        (*env)->ReleaseStringUTFChars(env, fallbackFontPath, font_path);
    }

    g_sub_engine = eng; // Store it globally
    return (jlong)(intptr_t)eng;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_destroy(get_engine(handle));
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
    // Extract the raw hardware window from the Java object
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;

    // STRICTLY filesv2: Only attach the surface here. Sizing happens in SurfaceChanged.
    sub_engine_attach_surface(get_engine(handle), window);

    if (window) {
        ANativeWindow_release(window);
    }
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height) {
    sub_engine_surface_resized(get_engine(handle), width, height);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_detach_surface(get_engine(handle));
}

// --- Stubbed Style Setters for Phase 1 ---
// You will map these to sub_style_set_* later when porting the UI settings.
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontSize(JNIEnv *env, jobject thiz, jlong handle, jfloat pt) {}
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint r, jint g, jint b, jint a) {}
// ... (Add the remaining stubs from jni_sub_engine.h here to satisfy the linker)
