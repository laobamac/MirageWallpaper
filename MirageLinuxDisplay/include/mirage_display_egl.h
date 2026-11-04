#ifndef MIRAGE_DISPLAY_EGL_H
#define MIRAGE_DISPLAY_EGL_H

#include "mirage_display.h"

#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

/*
 * Public C ABI for the EGL DMA-BUF import helper.
 *
 * Consumers with an EGL display import protocol buffer pools through
 * EGL_EXT_image_dma_buf_import and synchronize with native fences; the
 * acquire/release helpers consume their descriptors on every path.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* EGL DTOs use the same explicit C ABI layout as the core protocol types. */
#pragma pack(push, 8)

typedef struct md_egl_importer md_egl_importer_t;

typedef struct md_egl_context {
    EGLDisplay display;
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

/* The returned importer is caller-owned and destroyed with md_egl_importer_free(). */
md_egl_importer_t* md_egl_importer_new(const md_egl_context_t* context);
void md_egl_importer_free(md_egl_importer_t* importer);
md_result_t md_egl_importer_import_pool(md_egl_importer_t* importer,
                                        const md_buffer_pool_t* pool);
void md_egl_importer_release_pool(md_egl_importer_t* importer);
const md_egl_imported_pool_t* md_egl_importer_pool(const md_egl_importer_t* importer);

/* Consumes acquire_sync_fd and inserts a native-fence wait into the EGL stream. */
md_result_t md_egl_wait_acquire_sync(md_egl_importer_t* importer,
                                     int32_t acquire_sync_fd);
/* Consumes release_syncobj_fd and attaches a fence from the current GL context. */
md_result_t md_egl_release_after_current_context(md_egl_importer_t* importer,
                                                  int32_t release_syncobj_fd);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
