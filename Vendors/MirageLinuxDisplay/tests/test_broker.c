#define _GNU_SOURCE

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

typedef struct broker_thread {
    md_broker_t* broker;
    pthread_t thread;
} broker_thread_t;

typedef struct display_observer {
    unsigned connected;
    unsigned buffers;
    unsigned unbinds;
    unsigned configs;
    unsigned frames;
    uint64_t output_id;
    uint64_t expected_generation;
} display_observer_t;

typedef struct producer_observer {
    unsigned connected;
    unsigned configs;
    unsigned motion;
    uint64_t output_id;
} producer_observer_t;

static void* broker_main(void* opaque) {
    broker_thread_t* thread = opaque;
    for (;;) {
        int rc = md_broker_dispatch(thread->broker, 20);
        if (rc == MD_ERR_DISCONNECTED) break;
        assert(rc >= 0);
    }
    return NULL;
}

static void on_display_connected(void* opaque, uint64_t output_id) {
    display_observer_t* observer = opaque;
    assert(output_id != 0);
    observer->output_id = output_id;
    ++observer->connected;
}

static void on_display_buffers(void* opaque, const md_buffer_pool_t* pool) {
    display_observer_t* observer = opaque;
    assert(pool->generation == observer->expected_generation);
    assert(pool->buffer_count == 2);
    assert(pool->planes[0][0].fd >= 0);
    ++observer->buffers;
}

static void on_display_buffers_releasing(void* opaque, const md_buffer_pool_t* pool) {
    display_observer_t* observer = opaque;
    assert(pool->generation == observer->expected_generation);
    ++observer->unbinds;
}

static void on_display_config(void* opaque, const md_display_config_t* config) {
    display_observer_t* observer = opaque;
    assert(config->generation == observer->expected_generation);
    assert(config->destination.width == 1280.0f);
    ++observer->configs;
}

static void on_display_frame(void* opaque, const md_frame_t* frame) {
    display_observer_t* observer = opaque;
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
    producer_observer_t* observer = opaque;
    assert(producer_id != 0);
    assert(output_id != 0);
    observer->output_id = output_id;
    ++observer->connected;
}

static void on_producer_config(void* opaque, const md_producer_config_t* config) {
    producer_observer_t* observer = opaque;
    assert(config->physical_width == 1280);
    assert(config->physical_height == 720);
    assert(config->fourcc == UINT32_C(0x34325258));
    ++observer->configs;
}

static void on_producer_motion(void* opaque, const md_pointer_motion_t* event) {
    producer_observer_t* observer = opaque;
    assert(event->x == 10.0f);
    assert(event->y == 20.0f);
    ++observer->motion;
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
        struct pollfd descriptor = {
            .fd = md_display_get_fd(display),
            .events = POLLIN | (md_display_wants_writable(display) ? POLLOUT : 0),
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
                          unsigned configs, unsigned motion) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (observer->configs >= configs && observer->motion >= motion) return;
        struct pollfd descriptor = {
            .fd = md_producer_get_fd(producer),
            .events = POLLIN | (md_producer_wants_writable(producer) ? POLLOUT : 0),
            .revents = 0,
        };
        assert(poll(&descriptor, 1, 50) >= 0);
        if ((descriptor.revents & POLLIN) != 0) assert(md_producer_dispatch(producer) >= 0);
        if ((descriptor.revents & POLLOUT) != 0) assert(md_producer_handle_writable(producer) >= 0);
    }
    assert(observer->configs >= configs);
    assert(observer->motion >= motion);
}

static void pump_display_unbind(md_display_t* display, display_observer_t* observer,
                                unsigned unbinds) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (observer->unbinds >= unbinds && !md_display_wants_writable(display)) return;
        struct pollfd descriptor = {
            .fd = md_display_get_fd(display),
            .events = POLLIN | (md_display_wants_writable(display) ? POLLOUT : 0),
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
                    (long)getpid()) > 0);

    md_broker_options_t broker_options = {
        .socket_path = socket_path,
        .server_name = "test-broker",
        .server_version = "0.1",
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_POINTER_AXIS,
        .max_routes = 2,
    };
    md_broker_t* broker = md_broker_new(&broker_options);
    assert(broker != NULL);
    int listen_result = md_broker_listen(broker);
    if (listen_result != MD_OK && (errno == EPERM || errno == EACCES)) {
        md_broker_free(broker);
        return 77;
    }
    assert(listen_result == MD_OK);
    broker_thread_t broker_thread = {.broker = broker};
    assert(pthread_create(&broker_thread.thread, NULL, broker_main, &broker_thread) == 0);

    display_observer_t display_observer = {.expected_generation = 1};
    md_display_callbacks_t display_callbacks = {
        .on_connected = on_display_connected,
        .on_buffers_ready = on_display_buffers,
        .on_buffers_releasing = on_display_buffers_releasing,
        .on_config = on_display_config,
        .on_frame = on_display_frame,
        .on_disconnected = on_display_disconnected,
        .user_data = &display_observer,
    };
    md_display_t* display = md_display_new(&display_callbacks);
    assert(display != NULL);

    producer_observer_t producer_observer = {0};
    md_producer_callbacks_t producer_callbacks = {
        .on_connected = on_producer_connected,
        .on_output_config = on_producer_config,
        .on_pointer_motion = on_producer_motion,
        .on_disconnected = on_producer_disconnected,
        .user_data = &producer_observer,
    };
    md_producer_t* producer = md_producer_new(&producer_callbacks);
    assert(producer != NULL);

    md_format_cap_t format = {
        .fourcc = UINT32_C(0x34325258),
        .plane_count = 1,
        .modifier = 0,
    };
    md_output_info_t output = {
        .stable_id = "test-output",
        .name = "Test output",
        .physical_width = 1280,
        .physical_height = 720,
        .logical_width = 1280,
        .logical_height = 720,
        .scale_120 = 120,
        .refresh_mhz = 60000,
        .transform = MD_TRANSFORM_NORMAL,
        .input_caps = MD_INPUT_POINTER_ENTER_LEAVE | MD_INPUT_POINTER_MOTION |
                      MD_INPUT_POINTER_BUTTON | MD_INPUT_POINTER_AXIS |
                      MD_INPUT_NON_CONSUMING,
    };
    md_consumer_caps_t caps = {
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_POINTER_AXIS,
        .sync_caps = 1,
        .max_width = 4096,
        .max_height = 4096,
        .formats = &format,
        .format_count = 1,
    };
    md_producer_info_t producer_info = {
        .stable_output_id = "test-output",
        .kind = "test-renderer",
        .formats = &format,
        .format_count = 1,
    };

    assert(md_display_connect(display, socket_path, "test-display", "0.1", &output, &caps,
                              3000) == MD_OK);
    assert(display_observer.connected == 1);
    assert(md_producer_connect(producer, socket_path, "test-producer", "0.1", &producer_info,
                               3000) == MD_OK);
    assert(producer_observer.connected == 1);
    assert(producer_observer.output_id == display_observer.output_id);
    pump_producer(producer, &producer_observer, 1, 0);

    md_buffer_pool_t pool;
    memset(&pool, 0, sizeof(pool));
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
    pump_producer(producer, &producer_observer, 1, 1);

    display_observer_t mirror_observer = {.expected_generation = 1};
    md_display_callbacks_t mirror_callbacks = display_callbacks;
    mirror_callbacks.user_data = &mirror_observer;
    md_display_t* mirror_display = md_display_new(&mirror_callbacks);
    assert(mirror_display != NULL);
    assert(md_display_connect(mirror_display, socket_path, "test-display-mirror", "0.1",
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
    mirror_display = NULL;
    usleep(50000);

    const uint64_t stable_output_id = display_observer.output_id;
    md_display_free(display);
    display = NULL;
    usleep(50000);

    display_observer_t reconnected_observer = {.expected_generation = 1};
    md_display_callbacks_t reconnected_callbacks = display_callbacks;
    reconnected_callbacks.user_data = &reconnected_observer;
    display = md_display_new(&reconnected_callbacks);
    assert(display != NULL);
    assert(md_display_connect(display, socket_path, "test-display-restarted", "0.1",
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
    producer = NULL;
    pump_display_unbind(display, &reconnected_observer, 1);
    usleep(50000);

    reconnected_observer.expected_generation = 2;
    producer_observer_t restarted_producer_observer = {0};
    md_producer_callbacks_t restarted_producer_callbacks = producer_callbacks;
    restarted_producer_callbacks.user_data = &restarted_producer_observer;
    producer = md_producer_new(&restarted_producer_callbacks);
    assert(producer != NULL);
    assert(md_producer_connect(producer, socket_path, "test-producer-restarted", "0.1",
                               &producer_info, 3000) == MD_OK);
    assert(restarted_producer_observer.connected == 1);
    assert(restarted_producer_observer.output_id == stable_output_id);
    pump_producer(producer, &restarted_producer_observer, 1, 0);

    memset(&pool, 0, sizeof(pool));
    pool.generation = 2;
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
