#include "mirage_display_egl.h"

#include "common/util.hpp"

#include <GLES2/gl2.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>

#include <fcntl.h>

/*
 * EGL DMA-BUF import helper (include/mirage_display_egl.h) built on
 * EGL_EXT_image_dma_buf_import with native fence synchronization.
 *
 * The importer owns every EGLImage it creates and releases them when the pool is
 * replaced or freed; the acquire/release sync helpers consume their descriptors
 * on every path.
 */

struct md_egl_importer {
    md_egl_context_t context;
    /* Resolved once via eglGetProcAddress; NULL means the driver lacks the extension. */
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence_fd;
    md_egl_imported_pool_t pool;
    bool pool_active;
};

namespace {

constexpr std::size_t kAttributeCapacity = 64U;

void clear_pool(md_egl_imported_pool_t* const pool) {
    md_egl_imported_pool_t cleared{};
    for (EGLImageKHR& image : cleared.images) {
        image = EGL_NO_IMAGE_KHR;
    }
    *pool = cleared;
}

/*
 * EGL expects a terminated flat attribute list.  Keeping the terminator slot
 * reserved prevents malformed DMA-BUF imports when each plane contributes
 * several attributes.
 */
md_result_t append_attribute(std::array<EGLint, kAttributeCapacity>* const attributes,
                             std::size_t* const count, const EGLint key,
                             const EGLint value) {
    if (*count + 2U >= attributes->size()) {
        return MD_ERR_NOMEM;
    }
    (*attributes)[*count] = key;
    ++(*count);
    (*attributes)[*count] = value;
    ++(*count);
    return MD_OK;
}

EGLint modifier_part(const uint64_t modifier, const uint32_t shift) {
    const uint64_t mask = UINT64_C(0xffffffff);
    return static_cast<EGLint>(static_cast<uint32_t>((modifier >> shift) & mask));
}

/*
 * Release is a void C API, so destruction failures have no caller-visible
 * error channel.  The pool is still invalidated because retrying EGL object
 * destruction after its owning context changes would be less safe.
 */
md_result_t destroy_images(md_egl_importer_t* const importer) {
    md_result_t result = MD_OK;
    for (uint32_t index = 0U; index < importer->pool.buffer_count; ++index) {
        if (importer->pool.images[index] != EGL_NO_IMAGE_KHR) {
            const EGLBoolean destroyed =
                importer->destroy_image(importer->context.display, importer->pool.images[index]);
            if (destroyed != EGL_TRUE) {
                result = MD_ERR_IO;
            }
        }
    }
    clear_pool(&importer->pool);
    return result;
}

}  // namespace


/*
 * Resolves the KHR extension entry points lazily at construction; only
 * eglCreateImageKHR / eglDestroyImageKHR are mandatory, and their absence means
 * the driver does not support EGL_EXT_image_dma_buf_import.
 */
extern "C" md_egl_importer_t* md_egl_importer_new(const md_egl_context_t* const context) {
    if (context == nullptr || context->display == EGL_NO_DISPLAY) {
        return nullptr;
    }

    std::unique_ptr<md_egl_importer_t> importer{new (std::nothrow) md_egl_importer_t{}};
    if (!importer) {
        return nullptr;
    }

    importer->context = *context;
    importer->create_image = std::bit_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    importer->destroy_image = std::bit_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    importer->create_sync = std::bit_cast<PFNEGLCREATESYNCKHRPROC>(
        eglGetProcAddress("eglCreateSyncKHR"));
    importer->destroy_sync = std::bit_cast<PFNEGLDESTROYSYNCKHRPROC>(
        eglGetProcAddress("eglDestroySyncKHR"));
    importer->wait_sync =
        std::bit_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
    importer->dup_fence_fd = std::bit_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (importer->create_image == nullptr || importer->destroy_image == nullptr) {
        return nullptr;
    }
    clear_pool(&importer->pool);
    importer->pool_active = false;
    return importer.release();
}

extern "C" void md_egl_importer_release_pool(md_egl_importer_t* const importer) {
    if (importer == nullptr || !importer->pool_active) {
        return;
    }

    if (destroy_images(importer) != MD_OK) {
        /* See destroy_images(): this ABI cannot return cleanup failures. */
    }
    importer->pool_active = false;
}

extern "C" void md_egl_importer_free(md_egl_importer_t* const importer) {
    if (importer == nullptr) {
        return;
    }

    md_egl_importer_release_pool(importer);
    delete importer;
}

extern "C" const md_egl_imported_pool_t* md_egl_importer_pool(
    const md_egl_importer_t* const importer) {
    if (importer == nullptr || !importer->pool_active) {
        return nullptr;
    }
    return &importer->pool;
}


/*
 * Validates the protocol pool against EGLint bounds, then creates one
 * EGLImageKHR per buffer from the DMA-BUF descriptors.  The pool must not be
 * active; pool descriptors are borrowed and never closed by the importer.
 */
extern "C" md_result_t md_egl_importer_import_pool(md_egl_importer_t* const importer,
                                                     const md_buffer_pool_t* const pool) {
    if (importer == nullptr || pool == nullptr || importer->pool_active) {
        return MD_ERR_STATE;
    }
    if (pool->buffer_count < 2U || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count == 0U || pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        pool->width == 0U || pool->height == 0U ||
        pool->width > static_cast<uint32_t>(std::numeric_limits<EGLint>::max()) ||
        pool->height > static_cast<uint32_t>(std::numeric_limits<EGLint>::max()) ||
        pool->generation == 0U) {
        return MD_ERR_INVALID;
    }
    for (uint32_t buffer_index = 0U; buffer_index < pool->buffer_count; ++buffer_index) {
        for (uint32_t plane_index = 0U; plane_index < pool->plane_count; ++plane_index) {
            const md_plane_t& plane = pool->planes[buffer_index][plane_index];
            if (plane.fd < 0 || plane.stride == 0U ||
                plane.stride > static_cast<uint32_t>(std::numeric_limits<EGLint>::max()) ||
                plane.offset > static_cast<uint32_t>(std::numeric_limits<EGLint>::max())) {
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

    for (uint32_t buffer_index = 0U; buffer_index < pool->buffer_count; ++buffer_index) {
        std::array<EGLint, kAttributeCapacity> attributes{};
        std::size_t attribute_count = 0U;
        md_result_t result = append_attribute(&attributes, &attribute_count, EGL_WIDTH,
                                              static_cast<EGLint>(pool->width));
        if (result == MD_OK) {
            result = append_attribute(&attributes, &attribute_count, EGL_HEIGHT,
                                      static_cast<EGLint>(pool->height));
        }
        if (result == MD_OK) {
            result = append_attribute(&attributes, &attribute_count,
                                      EGL_LINUX_DRM_FOURCC_EXT,
                                      static_cast<EGLint>(pool->fourcc));
        }
        if (result != MD_OK) {
            if (destroy_images(importer) != MD_OK) {
                return MD_ERR_IO;
            }
            return result;
        }

        std::array<std::optional<mirage::UniqueFd>, MIRAGE_DISPLAY_MAX_PLANES>
            duplicated_fds{};
        for (uint32_t plane_index = 0U; plane_index < pool->plane_count; ++plane_index) {
            const md_plane_t& plane = pool->planes[buffer_index][plane_index];
            const int duplicated_fd = fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
            if (duplicated_fd < 0) {
                result = MD_ERR_IO;
                break;
            }
            duplicated_fds[plane_index].emplace(static_cast<int32_t>(duplicated_fd));

            const EGLint fd_key = static_cast<EGLint>(EGL_DMA_BUF_PLANE0_FD_EXT +
                                                      plane_index * 3U);
            const EGLint offset_key = static_cast<EGLint>(EGL_DMA_BUF_PLANE0_OFFSET_EXT +
                                                          plane_index * 3U);
            const EGLint pitch_key = static_cast<EGLint>(EGL_DMA_BUF_PLANE0_PITCH_EXT +
                                                         plane_index * 3U);
            result = append_attribute(&attributes, &attribute_count, fd_key,
                                      static_cast<EGLint>(duplicated_fds[plane_index]->get()));
            if (result == MD_OK) {
                result = append_attribute(&attributes, &attribute_count, offset_key,
                                          static_cast<EGLint>(plane.offset));
            }
            if (result == MD_OK) {
                result = append_attribute(&attributes, &attribute_count, pitch_key,
                                          static_cast<EGLint>(plane.stride));
            }
            if (result != MD_OK) {
                break;
            }

            /* The protocol's modifier is explicit even when its value is
             * zero.  Passing it verbatim preserves the negotiated layout;
             * direct EGL dispatch reports unsupported implementations rather
             * than guessing an implicit modifier. */
            const EGLint modifier_lo_key = static_cast<EGLint>(
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT + plane_index * 2U);
            const EGLint modifier_hi_key = static_cast<EGLint>(
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT + plane_index * 2U);
            result = append_attribute(&attributes, &attribute_count, modifier_lo_key,
                                      modifier_part(pool->modifier, 0U));
            if (result == MD_OK) {
                result = append_attribute(&attributes, &attribute_count, modifier_hi_key,
                                          modifier_part(pool->modifier, 32U));
            }
            if (result != MD_OK) {
                break;
            }
        }
        if (result != MD_OK) {
            if (destroy_images(importer) != MD_OK) {
                return MD_ERR_IO;
            }
            return result;
        }

        attributes[attribute_count] = EGL_NONE;
        importer->pool.images[buffer_index] = importer->create_image(
            importer->context.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
            attributes.data());
        /* eglCreateImageKHR borrows these duplicate descriptors during the
         * call.  The source pool remains owner of its originals. */
        if (importer->pool.images[buffer_index] == EGL_NO_IMAGE_KHR) {
            if (destroy_images(importer) != MD_OK) {
                return MD_ERR_IO;
            }
            return MD_ERR_UNSUPPORTED;
        }
    }

    importer->pool_active = true;
    return MD_OK;
}


/*
 * Imports the acquire sync_file as a native fence and inserts a wait into the
 * EGL stream so GL commands that follow cannot sample the frame before the
 * producer finished writing it.  Consumes acquire_sync_fd on every path.
 */
extern "C" md_result_t md_egl_wait_acquire_sync(md_egl_importer_t* const importer,
                                                 const int32_t acquire_sync_fd) {
    if (acquire_sync_fd < 0) {
        return MD_ERR_INVALID;
    }
    mirage::UniqueFd acquire_fd{acquire_sync_fd};
    if (importer == nullptr) {
        return MD_ERR_INVALID;
    }
    if (importer->create_sync == nullptr || importer->destroy_sync == nullptr ||
        importer->wait_sync == nullptr) {
        return MD_ERR_UNSUPPORTED;
    }

    const int duplicated_descriptor = fcntl(acquire_fd.get(), F_DUPFD_CLOEXEC, 0);
    if (duplicated_descriptor < 0) {
        return MD_ERR_IO;
    }
    mirage::UniqueFd duplicated_fd{static_cast<int32_t>(duplicated_descriptor)};
    std::array<EGLint, 3U> attributes{
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
        static_cast<EGLint>(duplicated_fd.get()),
        EGL_NONE,
    };
    /* The EGL sync consumes this duplicate only when construction succeeds.
     * Move ownership out before the call and reconstruct the RAII owner on
     * failure so every return path still consumes the caller's FD. */
    const int32_t transferred_descriptor = duplicated_fd.release();
    attributes[1U] = static_cast<EGLint>(transferred_descriptor);
    const EGLSyncKHR sync = importer->create_sync(importer->context.display,
                                                  EGL_SYNC_NATIVE_FENCE_ANDROID,
                                                  attributes.data());
    if (sync == EGL_NO_SYNC_KHR) {
        mirage::UniqueFd failed_transfer{transferred_descriptor};
        return MD_ERR_IO;
    }
    /* EGL owns the duplicated native-fence descriptor after successful sync
     * creation.  The caller's descriptor remains owned by acquire_fd. */

    const EGLint waited = importer->wait_sync(importer->context.display, sync, 0);
    const EGLBoolean destroyed = importer->destroy_sync(importer->context.display, sync);
    if (destroyed != EGL_TRUE) {
        return MD_ERR_IO;
    }
    return waited == EGL_TRUE ? MD_OK : MD_ERR_IO;
}


/*
 * Attaches a fence from the current GL context to the release syncobj after
 * the last GPU read, letting the producer recycle the slot.  Consumes
 * release_syncobj_fd on every path.
 */
extern "C" md_result_t md_egl_release_after_current_context(
    md_egl_importer_t* const importer, const int32_t release_syncobj_fd) {
    if (release_syncobj_fd < 0) {
        return MD_ERR_INVALID;
    }
    mirage::UniqueFd release_fd{release_syncobj_fd};
    if (importer == nullptr) {
        return MD_ERR_INVALID;
    }

    /* eglDupNativeFenceFDANDROID captures commands issued before this point. */
    const std::array<EGLint, 1U> attributes{EGL_NONE};
    if (importer->create_sync == nullptr || importer->destroy_sync == nullptr ||
        importer->dup_fence_fd == nullptr) {
        return MD_ERR_UNSUPPORTED;
    }
    const EGLSyncKHR sync = importer->create_sync(importer->context.display,
                                                  EGL_SYNC_NATIVE_FENCE_ANDROID,
                                                  attributes.data());
    if (sync == EGL_NO_SYNC_KHR) {
        return MD_ERR_IO;
    }
    glFlush();
    const EGLint sync_file_descriptor =
        importer->dup_fence_fd(importer->context.display, sync);
    const EGLBoolean destroyed = importer->destroy_sync(importer->context.display, sync);
    if (destroyed != EGL_TRUE || sync_file_descriptor < 0) {
        return MD_ERR_IO;
    }

    mirage::UniqueFd sync_file_fd{static_cast<int32_t>(sync_file_descriptor)};
    return md_display_release_after_sync_file(release_fd.release(), sync_file_fd.release());
}
