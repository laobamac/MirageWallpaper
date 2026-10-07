#include "mirage_display.h"

#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Standalone consumer example: connects to a mirage-display-v1 broker and
 * prints pool, config, and frame callbacks for protocol debugging without a
 * desktop environment.
 */

static volatile sig_atomic_t running = 1;

static void stop_running(int) {
    running = 0;
}

static void on_connected(void*, uint64_t output_id) {
    printf("connected output=%llu\n", (unsigned long long)output_id);
}

static void on_buffers_ready(void*, const md_buffer_pool_t* pool) {
    printf("buffers generation=%llu count=%u %ux%u fourcc=0x%08x modifier=0x%llx\n",
           (unsigned long long)pool->generation, pool->buffer_count, pool->width, pool->height,
           pool->fourcc, (unsigned long long)pool->modifier);
}

static void on_buffers_releasing(void*, const md_buffer_pool_t* pool) {
    printf("buffers releasing generation=%llu\n", (unsigned long long)pool->generation);
}

static void on_config(void*, const md_display_config_t* config) {
    printf("config generation=%llu destination=%.0fx%.0f+%.0f+%.0f\n",
           (unsigned long long)config->generation, config->destination.width,
           config->destination.height, config->destination.x, config->destination.y);
}

static void on_frame(void*, const md_frame_t* frame) {
    printf("frame generation=%llu buffer=%u sequence=%llu acquire=%d release=%d\n",
           (unsigned long long)frame->buffer_generation, frame->buffer_index,
           (unsigned long long)frame->sequence, frame->acquire_sync_fd,
           frame->release_syncobj_fd);
    assert(close(frame->acquire_sync_fd) == 0);
    assert(close(frame->release_syncobj_fd) == 0);
}

static void on_disconnected(void*, md_result_t reason, const char* message) {
    fprintf(stderr, "disconnected: %d %s\n", reason, message != NULL ? message : "");
    running = 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s SOCKET_PATH\n", argv[0]);
        return 2;
    }
    void (*const previous_sigint)(int) = signal(SIGINT, stop_running);
    void (*const previous_sigterm)(int) = signal(SIGTERM, stop_running);
    if (previous_sigint == SIG_ERR || previous_sigterm == SIG_ERR) {
        perror("signal");
        return 1;
    }

    md_display_callbacks_t callbacks{};
    callbacks.on_connected = on_connected;
    callbacks.on_buffers_ready = on_buffers_ready;
    callbacks.on_buffers_releasing = on_buffers_releasing;
    callbacks.on_config = on_config;
    callbacks.on_frame = on_frame;
    callbacks.on_disconnected = on_disconnected;
    md_display_t* display = md_display_new(&callbacks);
    if (display == NULL) return 1;

    md_format_cap_t format = {
        .fourcc = UINT32_C(0x34325258),
        .plane_count = 1,
        .modifier = 0,
    };
    md_output_info_t output{};
    output.stable_id = "headless-output";
    output.name = "Mirage headless consumer";
    output.physical_width = 1920U;
    output.physical_height = 1080U;
    output.logical_width = 1920U;
    output.logical_height = 1080U;
    output.scale_120 = 120U;
    output.refresh_mhz = 60000U;
    output.transform = MD_TRANSFORM_NORMAL;
    output.input_caps = MD_INPUT_POINTER_MOTION | MD_INPUT_POINTER_BUTTON |
                        MD_INPUT_POINTER_AXIS;
    md_consumer_caps_t caps{};
    caps.features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS;
    caps.sync_caps = 1U;
    caps.max_width = 8192U;
    caps.max_height = 8192U;
    caps.formats = &format;
    caps.format_count = 1U;

    md_result_t rc = md_display_connect(display, argv[1], "mirage-headless-consumer", "0.2",
                                        &output, &caps, 5000);
    if (rc != MD_OK) {
        fprintf(stderr, "connect failed: %d\n", rc);
        md_display_free(display);
        return 1;
    }

    while (running) {
        struct pollfd pfd = {.fd = md_display_get_fd(display), .events = POLLIN, .revents = 0};
        if (md_display_wants_writable(display)) pfd.events |= POLLOUT;
        int poll_rc;
        do { poll_rc = poll(&pfd, 1, 1000); } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc < 0) {
            perror("poll");
            break;
        }
        if (poll_rc == 0) continue;
        if ((pfd.revents & POLLOUT) != 0) (void)md_display_handle_writable(display);
        if ((pfd.revents & (POLLIN | POLLERR | POLLHUP)) != 0 &&
            md_display_dispatch(display) < 0) break;
    }
    md_display_close(display);
    md_display_free(display);
    return 0;
}
