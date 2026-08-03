#ifndef MIRAGE_DISPLAY_SYNC_FANOUT_H
#define MIRAGE_DISPLAY_SYNC_FANOUT_H

#include <stdint.h>

typedef struct md_sync_fanout md_sync_fanout_t;

/* The original fd is borrowed. Returned child fds transfer to the caller. */
int md_sync_fanout_create(int original_syncobj_fd, uint32_t child_count,
                          int* child_fds, md_sync_fanout_t** out_fanout);
/* Same operation, using the producer's DRM render node when it is known. */
int md_sync_fanout_create_on_node(int original_syncobj_fd, uint32_t child_count,
                                  uint32_t drm_major, uint32_t drm_minor,
                                  int* child_fds, md_sync_fanout_t** out_fanout);
/* Returns one when completed, zero while pending, or a negative md_result_t. */
int md_sync_fanout_poll(md_sync_fanout_t* fanout);
void md_sync_fanout_abandon(md_sync_fanout_t* fanout, uint32_t child_index);
void md_sync_fanout_free(md_sync_fanout_t* fanout);

/* Consumes release_syncobj_fd and signals it on the requested render node. */
int md_display_signal_release_syncobj_on_node(int release_syncobj_fd,
                                               uint32_t drm_major,
                                               uint32_t drm_minor);

#endif
