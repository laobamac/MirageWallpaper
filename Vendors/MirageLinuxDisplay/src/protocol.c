#include "protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MD_MAX_STRING_BYTES 4096u

static bool valid_utf8(const uint8_t* text, size_t size) {
    size_t i = 0;
    while (i < size) {
        uint8_t c = text[i++];
        if (c <= 0x7f) continue;
        unsigned remaining;
        uint32_t codepoint;
        if ((c & 0xe0u) == 0xc0u) {
            remaining = 1;
            codepoint = c & 0x1fu;
            if (codepoint < 2u) return false;
        } else if ((c & 0xf0u) == 0xe0u) {
            remaining = 2;
            codepoint = c & 0x0fu;
        } else if ((c & 0xf8u) == 0xf0u) {
            remaining = 3;
            codepoint = c & 0x07u;
        } else {
            return false;
        }
        if (i + remaining > size) return false;
        for (unsigned j = 0; j < remaining; ++j) {
            uint8_t next = text[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if ((remaining == 2 && codepoint < 0x800u) ||
            (remaining == 3 && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
    }
    return true;
}

void md_writer_init(md_writer_t* writer, uint8_t* data, size_t capacity) {
    writer->data = data;
    writer->capacity = capacity;
    writer->size = 0;
}

static int reserve(md_writer_t* writer, size_t size) {
    if (writer == NULL || size > writer->capacity - writer->size) return -ENOSPC;
    return 0;
}

int md_write_u16(md_writer_t* writer, uint16_t value) {
    if (reserve(writer, 2) != 0) return -ENOSPC;
    writer->data[writer->size++] = (uint8_t)(value & UINT16_C(0xff));
    writer->data[writer->size++] = (uint8_t)((value >> 8) & UINT16_C(0xff));
    return 0;
}

int md_write_u32(md_writer_t* writer, uint32_t value) {
    if (reserve(writer, 4) != 0) return -ENOSPC;
    for (unsigned i = 0; i < 4; ++i) {
        writer->data[writer->size++] = (uint8_t)((value >> (i * 8u)) & UINT32_C(0xff));
    }
    return 0;
}

int md_write_u64(md_writer_t* writer, uint64_t value) {
    if (reserve(writer, 8) != 0) return -ENOSPC;
    for (unsigned i = 0; i < 8; ++i) {
        writer->data[writer->size++] = (uint8_t)((value >> (i * 8u)) & UINT64_C(0xff));
    }
    return 0;
}

int md_write_f32(md_writer_t* writer, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return md_write_u32(writer, bits);
}

int md_write_bytes(md_writer_t* writer, const void* data, size_t size) {
    if (reserve(writer, size) != 0 || (size > 0 && data == NULL)) return -ENOSPC;
    if (size > 0) memcpy(writer->data + writer->size, data, size);
    writer->size += size;
    return 0;
}

int md_write_string(md_writer_t* writer, const char* value) {
    if (value == NULL) return -EINVAL;
    size_t length = strlen(value);
    if (length > MD_MAX_STRING_BYTES || !valid_utf8((const uint8_t*)value, length)) return -EINVAL;
    int rc = md_write_u32(writer, (uint32_t)length);
    return rc == 0 ? md_write_bytes(writer, value, length) : rc;
}

void md_reader_init(md_reader_t* reader, const uint8_t* data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->offset = 0;
}

static int take(md_reader_t* reader, size_t size, const uint8_t** data) {
    if (reader == NULL || size > reader->size - reader->offset) return -EPROTO;
    if (data != NULL) *data = reader->data + reader->offset;
    reader->offset += size;
    return 0;
}

int md_read_u16(md_reader_t* reader, uint16_t* value) {
    const uint8_t* data;
    if (value == NULL || take(reader, 2, &data) != 0) return -EPROTO;
    *value = (uint16_t)((uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8));
    return 0;
}

int md_read_u32(md_reader_t* reader, uint32_t* value) {
    const uint8_t* data;
    if (value == NULL || take(reader, 4, &data) != 0) return -EPROTO;
    *value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
             ((uint32_t)data[3] << 24);
    return 0;
}

int md_read_u64(md_reader_t* reader, uint64_t* value) {
    const uint8_t* data;
    if (value == NULL || take(reader, 8, &data) != 0) return -EPROTO;
    *value = 0;
    for (unsigned i = 0; i < 8; ++i) *value |= (uint64_t)data[i] << (i * 8u);
    return 0;
}

int md_read_f32(md_reader_t* reader, float* value) {
    uint32_t bits;
    if (value == NULL || md_read_u32(reader, &bits) != 0) return -EPROTO;
    memcpy(value, &bits, sizeof(bits));
    return 0;
}

int md_read_bytes(md_reader_t* reader, void* data, size_t size) {
    const uint8_t* source;
    if ((size > 0 && data == NULL) || take(reader, size, &source) != 0) return -EPROTO;
    if (size > 0) memcpy(data, source, size);
    return 0;
}

int md_read_string(md_reader_t* reader, char** value) {
    uint32_t length;
    const uint8_t* data;
    if (value == NULL || md_read_u32(reader, &length) != 0 || length > MD_MAX_STRING_BYTES ||
        take(reader, length, &data) != 0 || !valid_utf8(data, length)) return -EPROTO;
    char* result = malloc((size_t)length + 1u);
    if (result == NULL) return -ENOMEM;
    memcpy(result, data, length);
    result[length] = '\0';
    *value = result;
    return 0;
}

int md_reader_finish(const md_reader_t* reader) {
    return reader != NULL && reader->offset == reader->size ? 0 : -EPROTO;
}

#define WRITE(call) do { int md_rc_ = (call); if (md_rc_ != 0) return md_rc_; } while (0)
#define READ(call) do { int md_rc_ = (call); if (md_rc_ != 0) return md_rc_; } while (0)

int md_proto_encode_hello(md_writer_t* writer, uint32_t role, const char* name,
                          const char* version, uint64_t features) {
    if (role != 1 && role != 2) return -EINVAL;
    WRITE(md_write_u32(writer, role));
    WRITE(md_write_u16(writer, 0));
    WRITE(md_write_u16(writer, MIRAGE_DISPLAY_PROTOCOL_MINOR));
    WRITE(md_write_u64(writer, features));
    WRITE(md_write_string(writer, name));
    return md_write_string(writer, version);
}

int md_proto_encode_register_output(md_writer_t* writer, const md_output_info_t* output) {
    if (output == NULL || output->stable_id == NULL || output->name == NULL) return -EINVAL;
    WRITE(md_write_string(writer, output->stable_id));
    WRITE(md_write_string(writer, output->name));
    WRITE(md_write_u32(writer, output->physical_width));
    WRITE(md_write_u32(writer, output->physical_height));
    WRITE(md_write_u32(writer, output->logical_width));
    WRITE(md_write_u32(writer, output->logical_height));
    WRITE(md_write_u32(writer, output->scale_120));
    WRITE(md_write_u32(writer, output->refresh_mhz));
    WRITE(md_write_u32(writer, (uint32_t)output->transform));
    WRITE(md_write_u32(writer, output->drm_render_major));
    WRITE(md_write_u32(writer, output->drm_render_minor));
    return md_write_u64(writer, output->input_caps);
}

int md_proto_encode_update_output(md_writer_t* writer, const md_output_info_t* output) {
    if (output == NULL) return -EINVAL;
    WRITE(md_write_u32(writer, output->physical_width));
    WRITE(md_write_u32(writer, output->physical_height));
    WRITE(md_write_u32(writer, output->logical_width));
    WRITE(md_write_u32(writer, output->logical_height));
    WRITE(md_write_u32(writer, output->scale_120));
    WRITE(md_write_u32(writer, output->refresh_mhz));
    return md_write_u32(writer, (uint32_t)output->transform);
}

int md_proto_encode_consumer_caps(md_writer_t* writer, const md_consumer_caps_t* caps) {
    if (caps == NULL || caps->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        (caps->format_count > 0 && caps->formats == NULL)) return -EINVAL;
    WRITE(md_write_u64(writer, caps->sync_caps));
    WRITE(md_write_u64(writer, caps->color_caps));
    WRITE(md_write_u32(writer, caps->max_width));
    WRITE(md_write_u32(writer, caps->max_height));
    WRITE(md_write_bytes(writer, caps->device_uuid, sizeof(caps->device_uuid)));
    WRITE(md_write_bytes(writer, caps->driver_uuid, sizeof(caps->driver_uuid)));
    WRITE(md_write_u32(writer, caps->format_count));
    for (uint32_t i = 0; i < caps->format_count; ++i) {
        WRITE(md_write_u32(writer, caps->formats[i].fourcc));
        WRITE(md_write_u32(writer, caps->formats[i].plane_count));
        WRITE(md_write_u64(writer, caps->formats[i].modifier));
    }
    return 0;
}

int md_proto_encode_pointer_enter(md_writer_t* writer, float x, float y, uint64_t timestamp_us) {
    WRITE(md_write_f32(writer, x)); WRITE(md_write_f32(writer, y));
    return md_write_u64(writer, timestamp_us);
}

int md_proto_encode_pointer_leave(md_writer_t* writer, uint64_t timestamp_us) {
    return md_write_u64(writer, timestamp_us);
}

int md_proto_encode_pointer_motion(md_writer_t* writer, float x, float y, uint64_t timestamp_us,
                                   uint32_t modifiers) {
    WRITE(md_write_f32(writer, x)); WRITE(md_write_f32(writer, y));
    WRITE(md_write_u64(writer, timestamp_us));
    return md_write_u32(writer, modifiers);
}

int md_proto_encode_pointer_button(md_writer_t* writer, float x, float y, uint32_t button,
                                   md_button_state_t state, uint64_t timestamp_us,
                                   uint32_t modifiers) {
    WRITE(md_write_f32(writer, x)); WRITE(md_write_f32(writer, y));
    WRITE(md_write_u32(writer, button)); WRITE(md_write_u32(writer, (uint32_t)state));
    WRITE(md_write_u64(writer, timestamp_us));
    return md_write_u32(writer, modifiers);
}

int md_proto_encode_pointer_axis(md_writer_t* writer, float x, float y, float delta_x,
                                 float delta_y, md_axis_source_t source, uint64_t timestamp_us,
                                 uint32_t modifiers) {
    WRITE(md_write_f32(writer, x)); WRITE(md_write_f32(writer, y));
    WRITE(md_write_f32(writer, delta_x)); WRITE(md_write_f32(writer, delta_y));
    WRITE(md_write_u32(writer, (uint32_t)source)); WRITE(md_write_u64(writer, timestamp_us));
    return md_write_u32(writer, modifiers);
}

int md_proto_encode_u32(md_writer_t* writer, uint32_t value) { return md_write_u32(writer, value); }
int md_proto_encode_u64(md_writer_t* writer, uint64_t value) { return md_write_u64(writer, value); }

int md_proto_encode_register_producer(md_writer_t* writer, const md_producer_info_t* info) {
    if (info == NULL || info->stable_output_id == NULL || info->kind == NULL ||
        info->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        (info->format_count > 0 && info->formats == NULL)) return -EINVAL;
    WRITE(md_write_string(writer, info->stable_output_id));
    WRITE(md_write_string(writer, info->kind));
    WRITE(md_write_u32(writer, info->drm_render_major));
    WRITE(md_write_u32(writer, info->drm_render_minor));
    WRITE(md_write_bytes(writer, info->device_uuid, sizeof(info->device_uuid)));
    WRITE(md_write_bytes(writer, info->driver_uuid, sizeof(info->driver_uuid)));
    WRITE(md_write_u32(writer, info->format_count));
    for (uint32_t i = 0; i < info->format_count; ++i) {
        if (info->formats[i].plane_count == 0 ||
            info->formats[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES) return -EINVAL;
        WRITE(md_write_u32(writer, info->formats[i].fourcc));
        WRITE(md_write_u32(writer, info->formats[i].plane_count));
        WRITE(md_write_u64(writer, info->formats[i].modifier));
    }
    return 0;
}

int md_proto_encode_offer_buffers(md_writer_t* writer, const md_buffer_pool_t* pool) {
    if (pool == NULL || pool->buffer_count < 2 ||
        pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS || pool->plane_count == 0 ||
        pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES || pool->generation == 0) return -EINVAL;
    WRITE(md_write_u64(writer, pool->generation));
    WRITE(md_write_u32(writer, pool->buffer_count));
    WRITE(md_write_u32(writer, pool->width));
    WRITE(md_write_u32(writer, pool->height));
    WRITE(md_write_u32(writer, pool->fourcc));
    WRITE(md_write_u32(writer, pool->plane_count));
    WRITE(md_write_u64(writer, pool->modifier));
    WRITE(md_write_u32(writer, pool->buffer_count * pool->plane_count));
    for (uint32_t b = 0; b < pool->buffer_count; ++b) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            WRITE(md_write_u32(writer, pool->planes[b][p].stride));
            WRITE(md_write_u32(writer, pool->planes[b][p].offset));
            WRITE(md_write_u64(writer, pool->planes[b][p].size));
        }
    }
    return 0;
}

int md_proto_encode_producer_frame(md_writer_t* writer, uint64_t generation,
                                   uint32_t buffer_index, uint64_t sequence) {
    if (generation == 0) return -EINVAL;
    WRITE(md_write_u64(writer, generation));
    WRITE(md_write_u32(writer, buffer_index));
    WRITE(md_write_u32(writer, 0));
    return md_write_u64(writer, sequence);
}

int md_proto_encode_config(md_writer_t* writer, const md_display_config_t* config) {
    if (config == NULL || config->generation == 0 ||
        config->transform > MD_TRANSFORM_FLIPPED_270) return -EINVAL;
    WRITE(md_write_u64(writer, config->generation));
    WRITE(md_write_f32(writer, config->source.x));
    WRITE(md_write_f32(writer, config->source.y));
    WRITE(md_write_f32(writer, config->source.width));
    WRITE(md_write_f32(writer, config->source.height));
    WRITE(md_write_f32(writer, config->destination.x));
    WRITE(md_write_f32(writer, config->destination.y));
    WRITE(md_write_f32(writer, config->destination.width));
    WRITE(md_write_f32(writer, config->destination.height));
    WRITE(md_write_u32(writer, (uint32_t)config->transform));
    for (size_t i = 0; i < 4; ++i) WRITE(md_write_f32(writer, config->clear_color[i]));
    return 0;
}

int md_proto_decode_welcome(const uint8_t* data, size_t size, md_proto_welcome_t* welcome) {
    if (welcome == NULL) return -EINVAL;
    memset(welcome, 0, sizeof(*welcome));
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint16_t reserved;
    READ(md_read_u16(&reader, &welcome->selected_minor));
    READ(md_read_u16(&reader, &reserved));
    if (reserved != 0) return -EPROTO;
    READ(md_read_u64(&reader, &welcome->features));
    READ(md_read_string(&reader, &welcome->server_name));
    int rc = md_read_string(&reader, &welcome->server_version);
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0) md_proto_welcome_clear(welcome);
    return rc;
}

void md_proto_welcome_clear(md_proto_welcome_t* welcome) {
    if (welcome == NULL) return;
    free(welcome->server_name); free(welcome->server_version);
    memset(welcome, 0, sizeof(*welcome));
}

int md_proto_decode_error(const uint8_t* data, size_t size, md_proto_error_t* error) {
    if (error == NULL) return -EINVAL;
    memset(error, 0, sizeof(*error));
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t fatal;
    READ(md_read_u32(&reader, &error->code)); READ(md_read_u32(&reader, &fatal));
    error->fatal = fatal != 0;
    int rc = md_read_string(&reader, &error->message);
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0) md_proto_error_clear(error);
    return rc;
}

void md_proto_error_clear(md_proto_error_t* error) {
    if (error == NULL) return;
    free(error->message); memset(error, 0, sizeof(*error));
}

int md_proto_decode_output_accepted(const uint8_t* data, size_t size, uint64_t* output_id) {
    md_reader_t reader; md_reader_init(&reader, data, size);
    READ(md_read_u64(&reader, output_id));
    return md_reader_finish(&reader);
}

int md_proto_decode_bind_buffers(const uint8_t* data, size_t size, md_buffer_pool_t* pool) {
    if (pool == NULL) return -EINVAL;
    memset(pool, 0, sizeof(*pool));
    for (size_t b = 0; b < MIRAGE_DISPLAY_MAX_BUFFERS; ++b)
        for (size_t p = 0; p < MIRAGE_DISPLAY_MAX_PLANES; ++p) pool->planes[b][p].fd = -1;
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t descriptor_count;
    READ(md_read_u64(&reader, &pool->generation));
    READ(md_read_u32(&reader, &pool->buffer_count));
    READ(md_read_u32(&reader, &pool->width)); READ(md_read_u32(&reader, &pool->height));
    READ(md_read_u32(&reader, &pool->fourcc)); READ(md_read_u32(&reader, &pool->plane_count));
    READ(md_read_u64(&reader, &pool->modifier)); READ(md_read_u32(&reader, &descriptor_count));
    if (pool->buffer_count < 2 || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count < 1 || pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        descriptor_count != pool->buffer_count * pool->plane_count) return -EPROTO;
    for (uint32_t b = 0; b < pool->buffer_count; ++b) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            READ(md_read_u32(&reader, &pool->planes[b][p].stride));
            READ(md_read_u32(&reader, &pool->planes[b][p].offset));
            READ(md_read_u64(&reader, &pool->planes[b][p].size));
        }
    }
    return md_reader_finish(&reader);
}

static int read_rect(md_reader_t* reader, md_rect_t* rect) {
    READ(md_read_f32(reader, &rect->x)); READ(md_read_f32(reader, &rect->y));
    READ(md_read_f32(reader, &rect->width)); return md_read_f32(reader, &rect->height);
}

int md_proto_decode_config(const uint8_t* data, size_t size, md_display_config_t* config) {
    if (config == NULL) return -EINVAL;
    memset(config, 0, sizeof(*config));
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t transform;
    READ(md_read_u64(&reader, &config->generation)); READ(read_rect(&reader, &config->source));
    READ(read_rect(&reader, &config->destination)); READ(md_read_u32(&reader, &transform));
    if (transform > MD_TRANSFORM_FLIPPED_270) return -EPROTO;
    config->transform = (md_transform_t)transform;
    for (size_t i = 0; i < 4; ++i) READ(md_read_f32(&reader, &config->clear_color[i]));
    return md_reader_finish(&reader);
}

int md_proto_decode_frame(const uint8_t* data, size_t size, md_frame_t* frame) {
    if (frame == NULL) return -EINVAL;
    memset(frame, 0, sizeof(*frame)); frame->acquire_sync_fd = -1; frame->release_syncobj_fd = -1;
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t reserved;
    READ(md_read_u64(&reader, &frame->buffer_generation));
    READ(md_read_u32(&reader, &frame->buffer_index)); READ(md_read_u32(&reader, &reserved));
    if (reserved != 0) return -EPROTO;
    READ(md_read_u64(&reader, &frame->sequence));
    return md_reader_finish(&reader);
}

int md_proto_decode_unbind(const uint8_t* data, size_t size, uint64_t* generation) {
    return md_proto_decode_output_accepted(data, size, generation);
}

int md_proto_decode_producer_accepted(const uint8_t* data, size_t size, uint64_t* producer_id,
                                      uint64_t* output_id) {
    md_reader_t reader; md_reader_init(&reader, data, size);
    READ(md_read_u64(&reader, producer_id));
    READ(md_read_u64(&reader, output_id));
    return md_reader_finish(&reader);
}

int md_proto_decode_output_config(const uint8_t* data, size_t size,
                                  md_producer_config_t* config) {
    if (config == NULL) return -EINVAL;
    memset(config, 0, sizeof(*config));
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t transform;
    READ(md_read_u32(&reader, &config->physical_width));
    READ(md_read_u32(&reader, &config->physical_height));
    READ(md_read_u32(&reader, &config->refresh_mhz));
    READ(md_read_u32(&reader, &transform));
    READ(md_read_u32(&reader, &config->fourcc));
    READ(md_read_u32(&reader, &config->plane_count));
    READ(md_read_u64(&reader, &config->modifier));
    if (config->physical_width == 0 || config->physical_height == 0 ||
        transform > MD_TRANSFORM_FLIPPED_270 || config->plane_count == 0 ||
        config->plane_count > MIRAGE_DISPLAY_MAX_PLANES) return -EPROTO;
    config->transform = (md_transform_t)transform;
    return md_reader_finish(&reader);
}

int md_proto_decode_pointer_enter(const uint8_t* data, size_t size, md_pointer_enter_t* event) {
    if (event == NULL) return -EINVAL;
    md_reader_t reader; md_reader_init(&reader, data, size);
    READ(md_read_f32(&reader, &event->x)); READ(md_read_f32(&reader, &event->y));
    READ(md_read_u64(&reader, &event->timestamp_us));
    return md_reader_finish(&reader);
}

int md_proto_decode_pointer_leave(const uint8_t* data, size_t size, uint64_t* timestamp_us) {
    md_reader_t reader; md_reader_init(&reader, data, size);
    READ(md_read_u64(&reader, timestamp_us));
    return md_reader_finish(&reader);
}

int md_proto_decode_pointer_motion(const uint8_t* data, size_t size,
                                   md_pointer_motion_t* event) {
    if (event == NULL) return -EINVAL;
    md_reader_t reader; md_reader_init(&reader, data, size);
    READ(md_read_f32(&reader, &event->x)); READ(md_read_f32(&reader, &event->y));
    READ(md_read_u64(&reader, &event->timestamp_us));
    READ(md_read_u32(&reader, &event->modifiers));
    return md_reader_finish(&reader);
}

int md_proto_decode_pointer_button(const uint8_t* data, size_t size,
                                   md_pointer_button_t* event) {
    if (event == NULL) return -EINVAL;
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t state;
    READ(md_read_f32(&reader, &event->x)); READ(md_read_f32(&reader, &event->y));
    READ(md_read_u32(&reader, &event->button)); READ(md_read_u32(&reader, &state));
    READ(md_read_u64(&reader, &event->timestamp_us));
    READ(md_read_u32(&reader, &event->modifiers));
    if (state > MD_BUTTON_PRESSED) return -EPROTO;
    event->state = (md_button_state_t)state;
    return md_reader_finish(&reader);
}

int md_proto_decode_pointer_axis(const uint8_t* data, size_t size, md_pointer_axis_t* event) {
    if (event == NULL) return -EINVAL;
    md_reader_t reader; md_reader_init(&reader, data, size);
    uint32_t source;
    READ(md_read_f32(&reader, &event->x)); READ(md_read_f32(&reader, &event->y));
    READ(md_read_f32(&reader, &event->delta_x)); READ(md_read_f32(&reader, &event->delta_y));
    READ(md_read_u32(&reader, &source)); READ(md_read_u64(&reader, &event->timestamp_us));
    READ(md_read_u32(&reader, &event->modifiers));
    if (source > MD_AXIS_CONTINUOUS) return -EPROTO;
    event->source = (md_axis_source_t)source;
    return md_reader_finish(&reader);
}

#undef WRITE
#undef READ
