#ifndef MIRAGE_DISPLAY_COMMON_UTIL_H
#define MIRAGE_DISPLAY_COMMON_UTIL_H

#include "mirage_display.h"

#include <stddef.h>

/* NULL-safe duplication. Returns NULL on NULL input or allocation failure. */
char* md_strdup(const char* value);

/* Closes and resets each descriptor in the array. */
void md_close_fds(int* fds, size_t count);

/* Duplicates descriptors with F_DUPFD_CLOEXEC. Destinations are initialized
 * to -1 and every successfully duplicated entry is closed on failure. */
int md_duplicate_fds(const int* source, size_t count, int* destination);

/* Initializes a buffer pool with every plane FD set to -1. */
void md_init_pool(md_buffer_pool_t* pool);

/* Closes every plane FD in the pool and reinitializes it. */
void md_close_pool(md_buffer_pool_t* pool);

/* Maps a negative errno from the codec layer to an md_result_t. */
md_result_t md_map_io_error(int error);

#endif
