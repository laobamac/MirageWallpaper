#ifndef MIRAGE_DISPLAY_EGL_H
#define MIRAGE_DISPLAY_EGL_H

#include "mirage_display.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_egl_importer md_egl_importer_t;

typedef struct md_egl_context {
    EGLDisplay display;
    void* (*get_proc_address)(const char* name);
} md_egl_context_t;

typedef struct md_egl_imported_pool {
    uint64_t generation;
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
    EGLImageKHR images[MIRAGE_DISPLAY_MAX_BUFFERS];
} md_egl_imported_pool_t;

md_egl_importer_t* md_egl_importer_new(const md_egl_context_t* context);
void md_egl_importer_free(md_egl_importer_t* importer);
int md_egl_importer_import_pool(md_egl_importer_t* importer, const md_buffer_pool_t* pool);
void md_egl_importer_release_pool(md_egl_importer_t* importer);
const md_egl_imported_pool_t* md_egl_importer_pool(const md_egl_importer_t* importer);

/* Consumes acquire_sync_fd and inserts a native-fence wait into the EGL stream. */
int md_egl_wait_acquire_sync(md_egl_importer_t* importer, int acquire_sync_fd);
/* Consumes release_syncobj_fd and attaches a fence from the current GL context. */
int md_egl_release_after_current_context(md_egl_importer_t* importer,
                                          int release_syncobj_fd);

#ifdef __cplusplus
}
#endif

#endif
