#define _GNU_SOURCE

#include "mirage_display_egl.h"

#include <GLES2/gl2.h>

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef EGLImageKHR (*md_egl_create_image_fn)(EGLDisplay, EGLContext, EGLenum,
                                              EGLClientBuffer, const EGLint*);
typedef EGLBoolean (*md_egl_destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef EGLSyncKHR (*md_egl_create_sync_fn)(EGLDisplay, EGLenum, const EGLint*);
typedef EGLBoolean (*md_egl_destroy_sync_fn)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*md_egl_client_wait_sync_fn)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
typedef EGLBoolean (*md_egl_wait_sync_fn)(EGLDisplay, EGLSyncKHR, EGLint);
typedef EGLint (*md_egl_dup_fence_fd_fn)(EGLDisplay, EGLSyncKHR);

struct md_egl_importer {
    md_egl_context_t context;
    md_egl_create_image_fn create_image;
    md_egl_destroy_image_fn destroy_image;
    md_egl_create_sync_fn create_sync;
    md_egl_destroy_sync_fn destroy_sync;
    md_egl_client_wait_sync_fn client_wait_sync;
    md_egl_wait_sync_fn wait_sync;
    md_egl_dup_fence_fd_fn dup_fence_fd;
    md_egl_imported_pool_t pool;
    bool pool_active;
    bool has_modifier_import;
};

#define RESOLVE(importer, member, type, name) \
    do { \
        union { \
            void* object; \
            __eglMustCastToProperFunctionPointerType egl_function; \
            type function; \
        } md_proc_; \
        if ((importer)->context.get_proc_address != NULL) { \
            md_proc_.object = (importer)->context.get_proc_address(name); \
        } else { \
            md_proc_.egl_function = eglGetProcAddress(name); \
        } \
        (importer)->member = md_proc_.function; \
    } while (0)

static void clear_pool(md_egl_imported_pool_t* pool) {
    memset(pool, 0, sizeof(*pool));
    for (size_t i = 0; i < MIRAGE_DISPLAY_MAX_BUFFERS; ++i) pool->images[i] = EGL_NO_IMAGE_KHR;
}

static bool has_extension(const char* extensions, const char* target) {
    if (extensions == NULL || target == NULL || target[0] == '\0') return false;
    size_t target_size = strlen(target);
    const char* current = extensions;
    while ((current = strstr(current, target)) != NULL) {
        bool left = current == extensions || current[-1] == ' ';
        bool right = current[target_size] == '\0' || current[target_size] == ' ';
        if (left && right) return true;
        current += target_size;
    }
    return false;
}

md_egl_importer_t* md_egl_importer_new(const md_egl_context_t* context) {
    if (context == NULL || context->display == EGL_NO_DISPLAY) return NULL;
    md_egl_importer_t* importer = calloc(1, sizeof(*importer));
    if (importer == NULL) return NULL;
    importer->context = *context;
    RESOLVE(importer, create_image, md_egl_create_image_fn, "eglCreateImageKHR");
    RESOLVE(importer, destroy_image, md_egl_destroy_image_fn, "eglDestroyImageKHR");
    RESOLVE(importer, create_sync, md_egl_create_sync_fn, "eglCreateSyncKHR");
    RESOLVE(importer, destroy_sync, md_egl_destroy_sync_fn, "eglDestroySyncKHR");
    RESOLVE(importer, client_wait_sync, md_egl_client_wait_sync_fn, "eglClientWaitSyncKHR");
    RESOLVE(importer, wait_sync, md_egl_wait_sync_fn, "eglWaitSyncKHR");
    RESOLVE(importer, dup_fence_fd, md_egl_dup_fence_fd_fn, "eglDupNativeFenceFDANDROID");
    clear_pool(&importer->pool);
    const char* extensions = eglQueryString(context->display, EGL_EXTENSIONS);
    if (importer->create_image == NULL || importer->destroy_image == NULL ||
        !has_extension(extensions, "EGL_EXT_image_dma_buf_import")) {
        free(importer);
        return NULL;
    }
    importer->has_modifier_import =
        has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    return importer;
}

static void destroy_images(md_egl_importer_t* importer) {
    for (uint32_t i = 0; i < importer->pool.buffer_count; ++i) {
        if (importer->pool.images[i] != EGL_NO_IMAGE_KHR) {
            (void)importer->destroy_image(importer->context.display, importer->pool.images[i]);
        }
    }
    clear_pool(&importer->pool);
}

void md_egl_importer_release_pool(md_egl_importer_t* importer) {
    if (importer == NULL || !importer->pool_active) return;
    destroy_images(importer);
    importer->pool_active = false;
}

void md_egl_importer_free(md_egl_importer_t* importer) {
    if (importer == NULL) return;
    md_egl_importer_release_pool(importer);
    free(importer);
}

const md_egl_imported_pool_t* md_egl_importer_pool(const md_egl_importer_t* importer) {
    return importer != NULL && importer->pool_active ? &importer->pool : NULL;
}

static int append_attribute(EGLint* attributes, size_t capacity, size_t* size,
                            EGLint key, EGLint value) {
    if (*size + 2u >= capacity) return MD_ERR_NOMEM;
    attributes[(*size)++] = key;
    attributes[(*size)++] = value;
    return MD_OK;
}

static EGLint modifier_part(uint64_t modifier, unsigned shift) {
    uint32_t part = (uint32_t)((modifier >> shift) & UINT64_C(0xffffffff));
    return (EGLint)part;
}

int md_egl_importer_import_pool(md_egl_importer_t* importer, const md_buffer_pool_t* pool) {
    if (importer == NULL || pool == NULL || importer->pool_active) return MD_ERR_STATE;
    if (pool->buffer_count < 2 || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count == 0 || pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        pool->width == 0 || pool->height == 0 || pool->width > INT_MAX || pool->height > INT_MAX ||
        pool->generation == 0) return MD_ERR_INVALID;
    if (pool->modifier != 0 && !importer->has_modifier_import) return MD_ERR_UNSUPPORTED;
    for (uint32_t i = 0; i < pool->buffer_count; ++i) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            if (pool->planes[i][p].fd < 0 || pool->planes[i][p].stride == 0 ||
                pool->planes[i][p].stride > INT_MAX || pool->planes[i][p].offset > INT_MAX) {
                return MD_ERR_INVALID;
            }
        }
    }

    clear_pool(&importer->pool);
    importer->pool.generation = pool->generation;
    importer->pool.buffer_count = pool->buffer_count;
    importer->pool.width = pool->width;
    importer->pool.height = pool->height;
    importer->pool.fourcc = pool->fourcc;
    importer->pool.plane_count = pool->plane_count;
    importer->pool.modifier = pool->modifier;

    for (uint32_t b = 0; b < pool->buffer_count; ++b) {
        EGLint attributes[64];
        size_t size = 0;
        if (append_attribute(attributes, 64, &size, EGL_WIDTH, (EGLint)pool->width) != MD_OK ||
            append_attribute(attributes, 64, &size, EGL_HEIGHT, (EGLint)pool->height) != MD_OK ||
            append_attribute(attributes, 64, &size, EGL_LINUX_DRM_FOURCC_EXT,
                             (EGLint)pool->fourcc) != MD_OK) {
            destroy_images(importer);
            return MD_ERR_INVALID;
        }
        int duplicated[MIRAGE_DISPLAY_MAX_PLANES];
        for (uint32_t p = 0; p < pool->plane_count; ++p) duplicated[p] = -1;
        int rc = MD_OK;
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            duplicated[p] = fcntl(pool->planes[b][p].fd, F_DUPFD_CLOEXEC, 0);
            if (duplicated[p] < 0) { rc = MD_ERR_IO; break; }
            EGLint fd_key = (EGLint)(EGL_DMA_BUF_PLANE0_FD_EXT + p * 3u);
            EGLint offset_key = (EGLint)(EGL_DMA_BUF_PLANE0_OFFSET_EXT + p * 3u);
            EGLint pitch_key = (EGLint)(EGL_DMA_BUF_PLANE0_PITCH_EXT + p * 3u);
            if (append_attribute(attributes, 64, &size, fd_key, (EGLint)duplicated[p]) != MD_OK ||
                append_attribute(attributes, 64, &size, offset_key,
                                 (EGLint)pool->planes[b][p].offset) != MD_OK ||
                append_attribute(attributes, 64, &size, pitch_key,
                                 (EGLint)pool->planes[b][p].stride) != MD_OK) {
                rc = MD_ERR_INVALID;
                break;
            }
            if (importer->has_modifier_import &&
                (append_attribute(attributes, 64, &size,
                                  (EGLint)(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT + p * 2u),
                                  modifier_part(pool->modifier, 0)) != MD_OK ||
                 append_attribute(attributes, 64, &size,
                                  (EGLint)(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT + p * 2u),
                                  modifier_part(pool->modifier, 32)) != MD_OK)) {
                rc = MD_ERR_INVALID;
                break;
            }
        }
        if (rc != MD_OK) {
            for (uint32_t p = 0; p < pool->plane_count; ++p) {
                if (duplicated[p] >= 0) close(duplicated[p]);
            }
            destroy_images(importer);
            return rc;
        }
        attributes[size++] = EGL_NONE;
        importer->pool.images[b] = importer->create_image(importer->context.display,
                                                           EGL_NO_CONTEXT,
                                                           EGL_LINUX_DMA_BUF_EXT,
                                                           (EGLClientBuffer)0, attributes);
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            if (duplicated[p] >= 0) close(duplicated[p]);
        }
        if (importer->pool.images[b] == EGL_NO_IMAGE_KHR) {
            destroy_images(importer);
            return MD_ERR_UNSUPPORTED;
        }
    }
    importer->pool_active = true;
    return MD_OK;
}

int md_egl_wait_acquire_sync(md_egl_importer_t* importer, int acquire_sync_fd) {
    if (importer == NULL || acquire_sync_fd < 0) {
        if (acquire_sync_fd >= 0) close(acquire_sync_fd);
        return MD_ERR_INVALID;
    }
    if (importer->create_sync == NULL || importer->destroy_sync == NULL ||
        (importer->wait_sync == NULL && importer->client_wait_sync == NULL)) {
        close(acquire_sync_fd);
        return MD_ERR_UNSUPPORTED;
    }
    int duplicated = fcntl(acquire_sync_fd, F_DUPFD_CLOEXEC, 0);
    close(acquire_sync_fd);
    if (duplicated < 0) return MD_ERR_IO;
    EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, duplicated, EGL_NONE};
    EGLSyncKHR sync = importer->create_sync(importer->context.display,
                                             EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync == EGL_NO_SYNC_KHR) {
        close(duplicated);
        return MD_ERR_IO;
    }
    EGLBoolean waited = EGL_FALSE;
    if (importer->wait_sync != NULL) {
        waited = importer->wait_sync(importer->context.display, sync, 0);
    } else if (importer->client_wait_sync != NULL) {
        waited = importer->client_wait_sync(importer->context.display, sync, 0,
                                            EGL_FOREVER_KHR) == EGL_CONDITION_SATISFIED_KHR;
    }
    (void)importer->destroy_sync(importer->context.display, sync);
    return waited == EGL_TRUE ? MD_OK : MD_ERR_IO;
}

int md_egl_release_after_current_context(md_egl_importer_t* importer,
                                          int release_syncobj_fd) {
    if (importer == NULL || release_syncobj_fd < 0) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return MD_ERR_INVALID;
    }
    if (importer->create_sync == NULL || importer->destroy_sync == NULL ||
        importer->dup_fence_fd == NULL) {
        close(release_syncobj_fd);
        return MD_ERR_UNSUPPORTED;
    }
    /* eglDupNativeFenceFDANDROID captures commands issued before this point. */
    EGLint attributes[] = {EGL_NONE};
    EGLSyncKHR sync = importer->create_sync(importer->context.display,
                                             EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync == EGL_NO_SYNC_KHR) {
        close(release_syncobj_fd);
        return MD_ERR_IO;
    }
    glFlush();
    int sync_file_fd = importer->dup_fence_fd(importer->context.display, sync);
    (void)importer->destroy_sync(importer->context.display, sync);
    if (sync_file_fd < 0) {
        close(release_syncobj_fd);
        return MD_ERR_IO;
    }
    return md_display_release_after_sync_file(release_syncobj_fd, sync_file_fd);
}

#undef RESOLVE
