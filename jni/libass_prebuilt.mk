# libass-prebuilt.mk

# LOCAL_SRC_FILES must be strictly relative to LOCAL_PATH (which is 'jni').
# Do NOT prepend $(LOCAL_PATH)/ here, because NDK does it automatically!
LIBASS_DEPS_LIB := ../../prebuilt/libass-deps/$(TARGET_ARCH_ABI)/lib

# LOCAL_EXPORT_C_INCLUDES can safely use $(LOCAL_PATH)
LIBASS_DEPS_INC := $(LOCAL_PATH)/../../prebuilt/libass-deps/$(TARGET_ARCH_ABI)/include

# ── freetype ────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := freetype_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_DEPS_LIB)/libfreetype.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_DEPS_INC)/freetype2
include $(PREBUILT_STATIC_LIBRARY)

# ── fribidi ─────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := fribidi_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_DEPS_LIB)/libfribidi.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_DEPS_INC)
include $(PREBUILT_STATIC_LIBRARY)

# ── harfbuzz ─────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := harfbuzz_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_DEPS_LIB)/libharfbuzz.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_DEPS_INC)/harfbuzz
include $(PREBUILT_STATIC_LIBRARY)

# ── libass ───────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := ass_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_DEPS_LIB)/libass.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_DEPS_INC)
include $(PREBUILT_STATIC_LIBRARY)
