#include "mirage_display_egl.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

/*
 * Unit tests for the EGL DMA-BUF importer and native fence helpers.
 */

int main(void) {
    md_egl_context_t context = {
        .display = EGL_NO_DISPLAY,
    };
    assert(md_egl_importer_new(nullptr) == nullptr);
    assert(md_egl_importer_new(&context) == nullptr);
    return 0;
}
