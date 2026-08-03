#define _GNU_SOURCE

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char* md_strdup(const char* value) {
    if (value == NULL) return NULL;
    size_t size = strlen(value) + 1u;
    char* copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

void md_close_fds(int* fds, size_t count) {
    if (fds == NULL) return;
    for (size_t i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

int md_duplicate_fds(const int* source, size_t count, int* destination) {
    for (size_t i = 0; i < count; ++i) destination[i] = -1;
    for (size_t i = 0; i < count; ++i) {
        destination[i] = fcntl(source[i], F_DUPFD_CLOEXEC, 0);
        if (destination[i] < 0) {
            md_close_fds(destination, count);
            return MD_ERR_IO;
        }
    }
    return MD_OK;
}

void md_init_pool(md_buffer_pool_t* pool) {
    memset(pool, 0, sizeof(*pool));
    for (size_t b = 0; b < MIRAGE_DISPLAY_MAX_BUFFERS; ++b) {
        for (size_t p = 0; p < MIRAGE_DISPLAY_MAX_PLANES; ++p) {
            pool->planes[b][p].fd = -1;
        }
    }
}

void md_close_pool(md_buffer_pool_t* pool) {
    if (pool == NULL) return;
    for (uint32_t b = 0; b < pool->buffer_count; ++b) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            if (pool->planes[b][p].fd >= 0) close(pool->planes[b][p].fd);
            pool->planes[b][p].fd = -1;
        }
    }
    md_init_pool(pool);
}

md_result_t md_map_io_error(int error) {
    if (error == -ENOMEM) return MD_ERR_NOMEM;
    if (error == -EPROTO || error == -EMSGSIZE) return MD_ERR_PROTOCOL;
    if (error == -ECONNRESET || error == -EPIPE || error == -ENOTCONN) {
        return MD_ERR_DISCONNECTED;
    }
    return MD_ERR_IO;
}
