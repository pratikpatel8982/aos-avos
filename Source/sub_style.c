#include "sub_style.h"
#include <stdlib.h>

// Opaque struct definition for Phase 1
struct SUB_USER_STYLE {
    int placeholder;
};

SUB_USER_STYLE *sub_style_create(void) {
    return calloc(1, sizeof(SUB_USER_STYLE));
}

void sub_style_destroy(SUB_USER_STYLE *style) {
    if (style) {
        free(style);
    }
}

// Optional: If the linker complains about sub_style_snapshot, add this stub too:
SUB_STYLE sub_style_snapshot(const SUB_USER_STYLE *style) {
    SUB_STYLE empty = {0};
    return empty;
}

int sub_style_is_forced(const SUB_USER_STYLE *style) {
    return 0;
}
