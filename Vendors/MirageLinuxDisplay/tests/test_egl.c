#include "mirage_display_egl.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
    md_egl_context_t context = {
        .display = EGL_NO_DISPLAY,
        .get_proc_address = NULL,
    };
    assert(md_egl_importer_new(NULL) == NULL);
    assert(md_egl_importer_new(&context) == NULL);
    return 0;
}
