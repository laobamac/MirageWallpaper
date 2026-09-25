#include "mirage_display.h"
#include "mirage_display_broker.h"
#include "mirage_display_producer.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Integration tests for the broker routing core: peer validation, routing,
 * format negotiation, fanout, and buffer generation management.
 */

typedef struct broker_thread {
    md_broker_t* broker{};
    pthread_t thread{};
} broker_thread_t;

typedef struct display_observer {
    unsigned connected{};
    unsigned buffers{};
    unsigned unbinds{};
    unsigned configs{};
    unsigned frames{};
    uint64_t output_id{};
    uint64_t expected_generation{};
} display_observer_t;

typedef struct producer_observer {
    unsigned connected{};
    unsigned configs{};
    unsigned motion{};
    unsigned window_states{};
    uint32_t last_window_flags{};
    uint64_t output_id{};
} producer_observer_t;

static void* broker_main(void* opaque) {
    broker_thread_t* const thread = static_cast<broker_thread_t*>(opaque);
    for (;;) {
        int rc = md_broker_dispatch(thread->broker, 20);
        if (rc == MD_ERR_DISCONNECTED) break;
        assert(rc >= 0);
    }
    return nullptr;
}

static void on_display_connected(void* opaque, uint64_t output_id) {
    display_observer_t* const observer = static_cast<display_observer_t*>(opaque);
    assert(output_id != 0);
    observer->output_id = output_id;
    ++observer->connected;
}

static void on_display_buffers(void* opaque, const md_buffer_pool_t* pool) {
    display_observer_t* const observer = static_cast<display_observer_t*>(opaque);
    assert(pool->generation == observer->expected_generation);
    assert(pool->buffer_count == 2);
    assert(pool->planes[0][0].fd >= 0);
    ++observer->buffers;
}

static void on_display_buffers_releasing(void* opaque, const md_buffer_pool_t* pool) {
    display_observer_t* const observer = static_cast<display_observer_t*>(opaque);
    assert(pool->generation == observer->expected_generation);
    ++observer->unbinds;
}

static void on_display_config(void* opaque, const md_display_config_t* config) {
    display_observer_t* const observer = static_cast<display_observer_t*>(opaque);
    assert(config->generation == observer->expected_generation);
    assert(config->destination.width == 1280.0f);
    ++observer->configs;
}

static void on_display_frame(void* opaque, const md_frame_t* frame) {
    display_observer_t* const observer = static_cast<display_observer_t*>(opaque);
    assert(frame->buffer_generation == observer->expected_generation);
    assert(frame->buffer_index == 1);
    assert(frame->acquire_sync_fd >= 0);
    assert(frame->release_syncobj_fd >= 0);
    close(frame->acquire_sync_fd);
    close(frame->release_syncobj_fd);
    ++observer->frames;
}

static void on_display_disconnected(void* opaque, md_result_t reason, const char* message) {
    (void)opaque;
    (void)reason;
    (void)message;
}

static void on_producer_connected(void* opaque, uint64_t producer_id, uint64_t output_id) {
    producer_observer_t* const observer = static_cast<producer_observer_t*>(opaque);
    assert(producer_id != 0);
    assert(output_id != 0);
    observer->output_id = output_id;
    ++observer->connected;
}

static void on_producer_config(void* opaque, const md_producer_config_t* config) {
    producer_observer_t* const observer = static_cast<producer_observer_t*>(opaque);
    assert(config->physical_width == 1280);
    assert(config->physical_height == 720);
    assert(config->logical_width == 1280);
    assert(config->logical_height == 720);
    assert(config->fourcc == UINT32_C(0x34325258));
    /* The producer advertises a non-zero candidate first.  Exact tuple
     * matching must still select the display's explicit linear modifier. */
    assert(config->modifier == 0U);
    assert((config->target_gpu_flags & MD_TARGET_GPU_RENDER_NODE_VALID) != 0U);
    assert(config->target_drm_render_major == 226U);
    assert(config->target_drm_render_minor == 128U);
    ++observer->configs;
}

static void on_producer_motion(void* opaque, const md_pointer_motion_t* event) {
    producer_observer_t* const observer = static_cast<producer_observer_t*>(opaque);
    assert(event->x == 10.0f);
    assert(event->y == 20.0f);
    ++observer->motion;
}

static void on_producer_window_state(void* opaque, uint32_t flags) {
    producer_observer_t* const observer = static_cast<producer_observer_t*>(opaque);
    observer->last_window_flags = flags;
    ++observer->window_states;
}

/* Host-side window-state notification captured from the broker options hook;
 * the callback runs on the broker dispatch thread. */
static uint32_t g_host_window_flags = 0;
static char g_host_window_stable_id[64] = {0};
static void on_host_window_state(void* opaque, const char* stable_id, uint32_t flags) {
    (void)opaque;
    g_host_window_flags = flags;
    if (stable_id != nullptr) {
        strncpy(g_host_window_stable_id, stable_id, sizeof(g_host_window_stable_id) - 1);
    }
}

static void on_producer_disconnected(void* opaque, md_result_t reason, const char* message) {
    (void)opaque;
    (void)reason;
    (void)message;
}

static void pump_display(md_display_t* display, display_observer_t* observer,
                         unsigned buffers, unsigned configs, unsigned frames) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (observer->buffers >= buffers && observer->configs >= configs &&
            observer->frames >= frames) return;
        const short events = static_cast<short>(POLLIN |
            (md_display_wants_writable(display) != 0U ? POLLOUT : 0));
        struct pollfd descriptor{
            .fd = md_display_get_fd(display),
            .events = events,
            .revents = 0,
        };
        assert(poll(&descriptor, 1, 50) >= 0);
        if ((descriptor.revents & POLLIN) != 0) assert(md_display_dispatch(display) >= 0);
        if ((descriptor.revents & POLLOUT) != 0) assert(md_display_handle_writable(display) >= 0);
    }
    assert(observer->buffers >= buffers);
    assert(observer->configs >= configs);
    assert(observer->frames >= frames);
}

static void pump_producer(md_producer_t* producer, producer_observer_t* observer,
                          unsigned configs, unsigned motion, unsigned window_states) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (observer->configs >= configs && observer->motion >= motion &&
            observer->window_states >= window_states) return;
        const short events = static_cast<short>(POLLIN |
            (md_producer_wants_writable(producer) != 0U ? POLLOUT : 0));
        struct pollfd descriptor{
            .fd = md_producer_get_fd(producer),
            .events = events,
            .revents = 0,
        };
        assert(poll(&descriptor, 1, 50) >= 0);
        if ((descriptor.revents & POLLIN) != 0) assert(md_producer_dispatch(producer) >= 0);
        if ((descriptor.revents & POLLOUT) != 0) assert(md_producer_handle_writable(producer) >= 0);
    }
    assert(observer->configs >= configs);
    assert(observer->motion >= motion);
    assert(observer->window_states >= window_states);
}

static void pump_display_unbind(md_display_t* display, display_observer_t* observer,
                                unsigned unbinds) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (observer->unbinds >= unbinds && !md_display_wants_writable(display)) return;
        const short events = static_cast<short>(POLLIN |
            (md_display_wants_writable(display) != 0U ? POLLOUT : 0));
        struct pollfd descriptor{
            .fd = md_display_get_fd(display),
            .events = events,
            .revents = 0,
        };
        assert(poll(&descriptor, 1, 50) >= 0);
        if ((descriptor.revents & POLLIN) != 0) assert(md_display_dispatch(display) >= 0);
        if ((descriptor.revents & POLLOUT) != 0) assert(md_display_handle_writable(display) >= 0);
    }
    assert(observer->unbinds >= unbinds);
}

int main(void) {
    char socket_path[128];
    assert(snprintf(socket_path, sizeof(socket_path), "@mirage-display-broker-%ld",
                    static_cast<long>(getpid())) > 0);

    md_broker_options_t broker_options = {
        .socket_path = socket_path,
        .server_name = "test-broker",
        .server_version = "0.2",
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_POINTER_AXIS | MD_FEATURE_WINDOW_STATE,
        .max_routes = 2,
        .on_output_added = nullptr,
        .on_output_updated = nullptr,
        .on_output_removed = nullptr,
        .on_window_state = on_host_window_state,
        .user_data = nullptr,
    };
    md_broker_t* broker = md_broker_new(&broker_options);
    assert(broker != NULL);
    md_result_t listen_result = md_broker_listen(broker);
    if (listen_result != MD_OK && (errno == EPERM || errno == EACCES)) {
        md_broker_free(broker);
        return 77;
    }
    assert(listen_result == MD_OK);
    broker_thread_t broker_thread{};
    broker_thread.broker = broker;
    assert(pthread_create(&broker_thread.thread, NULL, broker_main, &broker_thread) == 0);

    display_observer_t display_observer{};
    display_observer.expected_generation = 1U;
    md_display_callbacks_t display_callbacks{};
    display_callbacks.on_connected = on_display_connected;
    display_callbacks.on_buffers_ready = on_display_buffers;
    display_callbacks.on_buffers_releasing = on_display_buffers_releasing;
    display_callbacks.on_config = on_display_config;
    display_callbacks.on_frame = on_display_frame;
    display_callbacks.on_disconnected = on_display_disconnected;
    display_callbacks.user_data = &display_observer;
    md_display_t* display = md_display_new(&display_callbacks);
    assert(display != NULL);

    producer_observer_t producer_observer{};
    md_producer_callbacks_t producer_callbacks{};
    producer_callbacks.on_connected = on_producer_connected;
    producer_callbacks.on_output_config = on_producer_config;
    producer_callbacks.on_pointer_motion = on_producer_motion;
    producer_callbacks.on_window_state = on_producer_window_state;
    producer_callbacks.on_disconnected = on_producer_disconnected;
    producer_callbacks.user_data = &producer_observer;
    md_producer_t* producer = md_producer_new(&producer_callbacks);
    assert(producer != NULL);

    md_format_cap_t format = {
        .fourcc = UINT32_C(0x34325258),
        .plane_count = 1,
        .modifier = 0,
    };
    md_format_cap_t producer_formats[] = {
        {.fourcc = format.fourcc, .plane_count = 1, .modifier = UINT64_C(0x100000000)},
        format,
    };
    md_output_info_t output{};
    output.stable_id = "test-output";
    output.name = "Test output";
    output.physical_width = 1280U;
    output.physical_height = 720U;
    output.logical_width = 1280U;
    output.logical_height = 720U;
    output.scale_120 = 120U;
    output.refresh_mhz = 60000U;
    output.transform = MD_TRANSFORM_NORMAL;
    /* The broker now forwards this consumer-owned render node before a
     * producer allocates or offers DMA-BUF pools. */
    output.drm_render_major = 226U;
    output.drm_render_minor = 128U;
    output.input_caps = MD_INPUT_POINTER_ENTER_LEAVE | MD_INPUT_POINTER_MOTION |
                        MD_INPUT_POINTER_BUTTON | MD_INPUT_POINTER_AXIS |
                        MD_INPUT_NON_CONSUMING;
    md_consumer_caps_t caps{};
    caps.features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_POINTER_AXIS | MD_FEATURE_WINDOW_STATE |
                    MD_FEATURE_TARGET_GPU_BINDING;
    caps.sync_caps = 1U;
    caps.max_width = 4096U;
    caps.max_height = 4096U;
    caps.formats = &format;
    caps.format_count = 1U;
    md_producer_info_t producer_info{};
    producer_info.stable_output_id = "test-output";
    producer_info.kind = "test-renderer";
    producer_info.formats = producer_formats;
    producer_info.format_count = 2U;

    assert(md_display_connect(display, socket_path, "test-display", "0.2", &output, &caps,
                              3000) == MD_OK);
    assert(display_observer.connected == 1);
    assert(md_producer_connect(producer, socket_path, "test-producer", "0.2", &producer_info,
                               3000) == MD_OK);
    assert(producer_observer.connected == 1);
    assert(producer_observer.output_id == display_observer.output_id);
    pump_producer(producer, &producer_observer, 1, 0, 1);
    /* The producer connected after the display, so it must have received the
     * cached default window state (0 = desktop focused) replayed together with
     * OUTPUT_CONFIG. */
    assert(producer_observer.window_states == 1);
    assert(producer_observer.last_window_flags == 0U);
    md_producer_gpu_info_t gpu{};
    gpu.drm_render_major = output.drm_render_major;
    gpu.drm_render_minor = output.drm_render_minor;
    assert(md_producer_bind_gpu(producer, &gpu) == MD_OK);

    md_buffer_pool_t pool{};
    pool.generation = 1;
    pool.buffer_count = 2;
    pool.width = 1280;
    pool.height = 720;
    pool.fourcc = format.fourcc;
    pool.plane_count = 1;
    for (uint32_t i = 0; i < pool.buffer_count; ++i) {
        pool.planes[i][0].fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(pool.planes[i][0].fd >= 0);
        pool.planes[i][0].stride = 5120;
        pool.planes[i][0].size = UINT64_C(3686400);
    }
    assert(md_producer_offer_buffers(producer, &pool) == MD_OK);
    for (uint32_t i = 0; i < pool.buffer_count; ++i) close(pool.planes[i][0].fd);
    pump_display(display, &display_observer, 1, 0, 0);

    md_display_config_t config = {
        .generation = 1,
        .source = {0.0f, 0.0f, 1280.0f, 720.0f},
        .destination = {0.0f, 0.0f, 1280.0f, 720.0f},
        .transform = MD_TRANSFORM_NORMAL,
        .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    assert(md_producer_set_config(producer, &config) == MD_OK);
    pump_display(display, &display_observer, 1, 1, 0);

    int acquire_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int release_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(acquire_fd >= 0 && release_fd >= 0);
    assert(md_producer_submit_frame(producer, 1, 1, 5, acquire_fd, release_fd) == MD_OK);
    pump_display(display, &display_observer, 1, 1, 1);

    assert(md_display_send_pointer_motion(display, 10.0f, 20.0f, 100, 0) == MD_OK);
    pump_producer(producer, &producer_observer, 1, 1, 1);

    /* WINDOW_STATE from the display is forwarded to the producer verbatim. */
    assert(md_display_send_window_state(display, 0xAU) == MD_OK);
    pump_producer(producer, &producer_observer, 1, 1, 2);
    assert(producer_observer.last_window_flags == 0xAU);
    /* The broker also notifies the embedding host with the route's stable id. */
    assert(g_host_window_flags == 0xAU);
    assert(strcmp(g_host_window_stable_id, "test-output") == 0);

    display_observer_t mirror_observer{};
    mirror_observer.expected_generation = 1U;
    md_display_callbacks_t mirror_callbacks = display_callbacks;
    mirror_callbacks.user_data = &mirror_observer;
    md_display_t* mirror_display = md_display_new(&mirror_callbacks);
    assert(mirror_display != NULL);
    assert(md_display_connect(mirror_display, socket_path, "test-display-mirror", "0.2",
                              &output, &caps, 3000) == MD_OK);
    assert(mirror_observer.output_id == display_observer.output_id);
    pump_display(mirror_display, &mirror_observer, 1, 1, 0);

    acquire_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    release_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(acquire_fd >= 0 && release_fd >= 0);
    assert(md_producer_submit_frame(producer, 1, 1, 6, acquire_fd, release_fd) == MD_OK);
    pump_display(display, &display_observer, 1, 1, 2);
    assert(mirror_observer.frames == 0);

    md_display_free(mirror_display);
    usleep(50000);

    const uint64_t stable_output_id = display_observer.output_id;
    md_display_free(display);
    usleep(50000);

    display_observer_t reconnected_observer{};
    reconnected_observer.expected_generation = 1U;
    md_display_callbacks_t reconnected_callbacks = display_callbacks;
    reconnected_callbacks.user_data = &reconnected_observer;
    display = md_display_new(&reconnected_callbacks);
    assert(display != NULL);
    assert(md_display_connect(display, socket_path, "test-display-restarted", "0.2",
                              &output, &caps, 3000) == MD_OK);
    assert(reconnected_observer.connected == 1);
    assert(reconnected_observer.output_id == stable_output_id);
    pump_display(display, &reconnected_observer, 1, 0, 0);

    acquire_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    release_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(acquire_fd >= 0 && release_fd >= 0);
    assert(md_producer_submit_frame(producer, 1, 1, 7, acquire_fd, release_fd) == MD_OK);
    pump_display(display, &reconnected_observer, 1, 0, 1);

    md_producer_free(producer);
    pump_display_unbind(display, &reconnected_observer, 1);
    usleep(50000);

    reconnected_observer.expected_generation = 2U;
    producer_observer_t restarted_producer_observer{};
    md_producer_callbacks_t restarted_producer_callbacks = producer_callbacks;
    restarted_producer_callbacks.user_data = &restarted_producer_observer;
    producer = md_producer_new(&restarted_producer_callbacks);
    assert(producer != NULL);
    assert(md_producer_connect(producer, socket_path, "test-producer-restarted", "0.2",
                               &producer_info, 3000) == MD_OK);
    assert(restarted_producer_observer.connected == 1);
    assert(restarted_producer_observer.output_id == stable_output_id);
    pump_producer(producer, &restarted_producer_observer, 1, 0, 1);
    /* A restarted producer replays the last cached window state (0xA from the
     * forwarding test above) so it does not wait for the next desktop change. */
    assert(restarted_producer_observer.last_window_flags == 0xAU);
    assert(md_producer_bind_gpu(producer, &gpu) == MD_OK);

    md_buffer_pool_t replacement_pool{};
    replacement_pool.generation = 2U;
    replacement_pool.buffer_count = 2U;
    replacement_pool.width = 1280U;
    replacement_pool.height = 720U;
    replacement_pool.fourcc = format.fourcc;
    replacement_pool.plane_count = 1U;
    for (uint32_t i = 0U; i < replacement_pool.buffer_count; ++i) {
        replacement_pool.planes[i][0].fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(replacement_pool.planes[i][0].fd >= 0);
        replacement_pool.planes[i][0].stride = 5120U;
        replacement_pool.planes[i][0].size = UINT64_C(3686400);
    }
    assert(md_producer_offer_buffers(producer, &replacement_pool) == MD_OK);
    for (uint32_t i = 0U; i < replacement_pool.buffer_count; ++i) {
        assert(close(replacement_pool.planes[i][0].fd) == 0);
    }
    pump_display(display, &reconnected_observer, 2, 0, 1);

    config.generation = 2;
    assert(md_producer_set_config(producer, &config) == MD_OK);
    pump_display(display, &reconnected_observer, 2, 2, 1);

    acquire_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    release_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(acquire_fd >= 0 && release_fd >= 0);
    assert(md_producer_submit_frame(producer, 2, 1, 8, acquire_fd, release_fd) == MD_OK);
    pump_display(display, &reconnected_observer, 2, 2, 2);

    md_display_free(display);
    md_producer_free(producer);
    md_broker_stop(broker);
    assert(pthread_join(broker_thread.thread, NULL) == 0);
    md_broker_free(broker);
    return 0;
}
