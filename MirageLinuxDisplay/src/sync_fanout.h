#ifndef MIRAGE_DISPLAY_SYNC_FANOUT_H
#define MIRAGE_DISPLAY_SYNC_FANOUT_H

#include "mirage_display.h"

#include <stdint.h>

/*
 * C ABI for the broker-side DRM syncobj fanout.
 *
 * The original syncobj descriptor is borrowed by create; on MD_OK each child
 * descriptor transfers to the caller and out_fanout owns the fanout until
 * md_sync_fanout_free.  Restricted to the broker dispatch thread.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_sync_fanout md_sync_fanout_t;

/*
 * The original syncobj FD is borrowed. On MD_OK each child FD transfers to
 * the caller, and out_fanout transfers a broker-owned fanout that must be
 * released with md_sync_fanout_free. child_fds must hold child_count entries;
 * neither pointer may be null. These calls are restricted to the broker
 * dispatch thread because the fanout state is not internally synchronized.
 */
int md_sync_fanout_create(int original_syncobj_fd, uint32_t child_count,
                          int* child_fds, md_sync_fanout_t** out_fanout);
/* Same operation using the producer's explicit DRM render node. */
int md_sync_fanout_create_on_node(int original_syncobj_fd, uint32_t child_count,
                                  uint32_t drm_major, uint32_t drm_minor,
                                  int* child_fds, md_sync_fanout_t** out_fanout);
/* Returns one when completed, zero while pending, or a negative md_result_t. */
int md_sync_fanout_poll(md_sync_fanout_t* fanout);
/* Marks one consumer as unavailable; fanout remains owned by the caller. */
void md_sync_fanout_abandon(md_sync_fanout_t* fanout, uint32_t child_index);
/* Cancels a pending fanout, signals its producer syncobj when possible, then frees it. */
void md_sync_fanout_free(md_sync_fanout_t* fanout);

/*
 * Consumes release_syncobj_fd on every path and signals it on the requested
 * render node. Returns MD_OK or a negative md_result_t; not thread-safe with
 * other users of the same DRM syncobj.
 */
md_result_t md_display_signal_release_syncobj_on_node(int32_t release_syncobj_fd,
                                                       uint32_t drm_major,
                                                       uint32_t drm_minor);

#ifdef __cplusplus
}
#endif

#endif
