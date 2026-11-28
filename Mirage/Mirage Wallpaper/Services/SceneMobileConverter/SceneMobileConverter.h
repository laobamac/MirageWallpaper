//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_CONVERTER_H
#define MIRAGE_SCENE_MOBILE_CONVERTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MirageMobileStatus : int32_t {
    MIRAGE_MOBILE_OK = 0,
    MIRAGE_MOBILE_CANCELLED = 1,
    MIRAGE_MOBILE_INVALID_PROJECT = 2,
    MIRAGE_MOBILE_UNSAFE_PROJECT_PATH = 3,
    MIRAGE_MOBILE_INVALID_PACKAGE = 4,
    MIRAGE_MOBILE_TRUNCATED_PACKAGE = 5,
    MIRAGE_MOBILE_UNSUPPORTED_PACKAGE_VERSION = 6,
    MIRAGE_MOBILE_INVALID_TEXTURE = 7,
    MIRAGE_MOBILE_UNSUPPORTED_TEXTURE_LAYOUT = 8,
    MIRAGE_MOBILE_UNSUPPORTED_TEXTURE_FORMAT = 9,
    MIRAGE_MOBILE_WALLPAPER_TOO_LARGE = 10,
    MIRAGE_MOBILE_IO_ERROR = 11,
} MirageMobileStatus;

typedef enum MirageMobileCompressionMode : int32_t {
    MIRAGE_MOBILE_COMPRESSION_HIGH_QUALITY = 0,
    MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE = 1,
} MirageMobileCompressionMode;

typedef void (*MirageMobileProgressCallback)(double fraction, void* context);

typedef struct MirageSceneMobileExportOptions {
    const char* wallpaper_dir;
    const char* render_dir;
    const char* const* overlay_dirs;
    size_t overlay_dir_count;
    const char* output_path;
    const char* zcompat_dir;
    const char* property_overrides_json;
    const char* workshop_url_override;
    uint64_t workshop_id_override;
    int32_t texture_reduction;        /* 0 = auto, 1/2/4/8 = explicit divisor */
    int32_t pixel_art;                /* -1 = auto, 0 = off, 1 = on */
    int32_t compression_mode;         /* -1 = auto, 0 = HighQuality, 1 = HighPerformance */
    int32_t keep_sounds;              /* 0 = strip sounds, 1 = keep sounds */
    int32_t force_legacy_pkg;         /* 0 = PKGV<=16 -> PKGM0014, 1 = PKGM0020 */
    int32_t prefer_rgb8_when_opaque;  /* 0 = always ETC2_RGBA8 (5), 1 = ETC2_RGB8 (3) when opaque */
    int32_t jobs;                     /* 0 = auto */
    MirageMobileProgressCallback progress_cb;
    void* progress_context;
    const volatile int32_t* cancel_flag;
} MirageSceneMobileExportOptions;

typedef struct MirageSceneMobileExportResult {
    int32_t status;
    int32_t detail_int;
    char* error_detail;
    char mpkg_magic[16];
    uint64_t total_entries;
    uint64_t tex_transcoded;
    uint64_t tex_skipped;
    uint64_t shaders_rewritten;
    uint64_t shaders_total;
    uint64_t sounds_removed;
    uint64_t output_size_bytes;
} MirageSceneMobileExportResult;

MirageSceneMobileExportResult MirageSceneMobileExport(
    const MirageSceneMobileExportOptions* options);

/* Primitive-parameter C ABI wrapper for Swift @_silgen_name without a bridging header. */
int32_t MirageSceneMobileExportFlat(
    const char* wallpaper_dir,
    const char* render_dir,
    const char* const* overlay_dirs,
    size_t overlay_dir_count,
    const char* output_path,
    const char* zcompat_dir,
    const char* property_overrides_json,
    int32_t texture_reduction,
    int32_t pixel_art,
    int32_t compression_mode,
    int32_t keep_sounds,
    MirageMobileProgressCallback progress_cb,
    void* progress_context,
    const volatile int32_t* cancel_flag,
    int32_t* out_detail_int,
    char** out_error_detail);

void MirageSceneMobileFreeString(char* value);

#ifdef __cplusplus
}
#endif

#endif /* MIRAGE_SCENE_MOBILE_CONVERTER_H */
