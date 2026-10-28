// Mirage Wallpaper
// Copyright © 2026 王孝慈. All rights reserved.

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>

// These offsets describe the pinned Intel Wine binary only. Native ARM64
// Wine must never install interpositions based on another architecture's ABI.
#if defined(__x86_64__)

// Wine 11.18 has inversions among the user, driver, client-surface and window-
// surface locks during SetWindowPos, GPU presentation and surface flushing.
// Acquire Wine's recursive user lock before each of these specific locks;
// preserve user_lock bookkeeping and trylock's nonblocking behavior. Dynamic
// surface mutexes are registered at their verified creation call site. Do not
// extend this ordering to unrelated pthread locks. Exact binary UUIDs only.
static pthread_mutex_t *driver_mutex;
static pthread_mutex_t *client_mutex;
static pthread_mutex_t *user_mutex;
static uintptr_t user_base;
static _Atomic(uintptr_t) surface_mutexes[2048];
static void (*user_lock)(void);
static void (*user_unlock)(void);
static __thread unsigned acquired_user_locks;
static int enabled;

static unsigned mutex_hash(pthread_mutex_t *mutex) { return (((uintptr_t)mutex >> 4) * 2654435761u) & 2047; }
static int known_surface(pthread_mutex_t *mutex) {
    unsigned first = mutex_hash(mutex);
    for (unsigned i = 0; i < 2048; ++i) {
        uintptr_t value = atomic_load(&surface_mutexes[(first + i) & 2047]);
        if (!value) return 0;
        if (value == (uintptr_t)mutex) return 1;
    }
    return 0;
}
static int should_order(pthread_mutex_t *mutex) {
    return enabled && user_lock && user_unlock && mutex != user_mutex &&
        (mutex == driver_mutex || mutex == client_mutex || known_surface(mutex));
}
static int tracked_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) {
    int result = pthread_mutex_init(mutex, attributes);
    if (!result && enabled && (uintptr_t)__builtin_return_address(0) == user_base + 0x14a53) {
        unsigned first = mutex_hash(mutex);
        for (unsigned i = 0; i < 2048; ++i) {
            _Atomic(uintptr_t) *slot = &surface_mutexes[(first + i) & 2047];
            uintptr_t value = atomic_load(slot);
            if (value <= 1 && atomic_compare_exchange_strong(slot, &value, (uintptr_t)mutex)) break;
        }
    }
    return result;
}
static int tracked_destroy(pthread_mutex_t *mutex) {
    int result = pthread_mutex_destroy(mutex);
    if (!result && enabled) {
        unsigned first = mutex_hash(mutex);
        for (unsigned i = 0; i < 2048; ++i) {
            _Atomic(uintptr_t) *slot = &surface_mutexes[(first + i) & 2047];
            uintptr_t value = atomic_load(slot);
            if (!value) break;
            if (value == (uintptr_t)mutex && atomic_compare_exchange_strong(slot, &value, 1)) break;
        }
    }
    return result;
}

static int ordered_lock(pthread_mutex_t *mutex) {
    int ordered = should_order(mutex);
    if (ordered) user_lock();
    int result = pthread_mutex_lock(mutex);
    if (ordered) {
        if (result) user_unlock();
        else ++acquired_user_locks;
    }
    return result;
}

static int ordered_trylock(pthread_mutex_t *mutex) {
    int ordered = should_order(mutex);
    if (ordered) {
        if (pthread_mutex_trylock(user_mutex)) return EBUSY;
        user_lock();
        pthread_mutex_unlock(user_mutex);
    }
    int result = pthread_mutex_trylock(mutex);
    if (ordered) {
        if (result) user_unlock();
        else ++acquired_user_locks;
    }
    return result;
}

static int ordered_unlock(pthread_mutex_t *mutex) {
    int ordered = should_order(mutex) && acquired_user_locks;
    int result = pthread_mutex_unlock(mutex);
    if (ordered && !result) {
        --acquired_user_locks;
        user_unlock();
    }
    return result;
}

__attribute__((used)) static struct { const void *replacement; const void *original; }
interpositions[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)ordered_lock, (const void *)pthread_mutex_lock},
    {(const void *)ordered_unlock, (const void *)pthread_mutex_unlock},
    {(const void *)ordered_trylock, (const void *)pthread_mutex_trylock},
    {(const void *)tracked_init, (const void *)pthread_mutex_init},
    {(const void *)tracked_destroy, (const void *)pthread_mutex_destroy},
};

static void image_added(const struct mach_header *header, intptr_t slide) {
    if (!enabled || header->magic != MH_MAGIC_64) return;
    const unsigned char driver_id[16] = {0xb0,0x54,0x0d,0x62,0xea,0x42,0x39,0x7c,0x8f,0x2d,0x69,0x12,0xe9,0x35,0x43,0xc6};
    const unsigned char user_id[16] = {0x12,0x22,0x35,0xc3,0xa6,0xea,0x3e,0xc2,0x8c,0x2d,0x95,0x9d,0x53,0x20,0x51,0x12};
    const struct load_command *command = (const void *)((const struct mach_header_64 *)header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_UUID) {
            const struct uuid_command *uuid = (const void *)command;
            if (!memcmp(uuid->uuid, driver_id, 16)) driver_mutex = (void *)(slide + 0x59f38);
            if (!memcmp(uuid->uuid, user_id, 16)) {
                user_base = slide;
                client_mutex = (void *)(slide + 0x1f12c0);
                user_mutex = (void *)(slide + 0x21e3a0);
                user_lock = (void *)(slide + 0x13d2d0);
                user_unlock = (void *)(slide + 0x13d310);
            }
        }
        command = (const void *)((const char *)command + command->cmdsize);
    }
    if (driver_mutex && user_lock && user_unlock) {
        static int logged;
        if (!logged++) fprintf(stderr,"[Mirage compositor] Wine 11.18 window and surface lock ordering enabled\n");
    }
}

__attribute__((constructor)) static void initialize_window_locks(void) {
    char path[4096]; uint32_t capacity = sizeof(path);
    if (_NSGetExecutablePath(path, &capacity) || !strstr(path, "Mirage WE Editor.app/")) return;
    enabled = 1;
    _dyld_register_func_for_add_image(image_added);
}
#endif
