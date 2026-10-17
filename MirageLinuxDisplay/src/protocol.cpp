#include "protocol.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>

/*
 * Per-message encode/decode for mirage-display-v1.
 *
 * This is the single place where a protocol field maps to its wire type, so the
 * golden-vector tests pin every field order and width here.  Decoders reject
 * trailing bytes and out-of-range counts rather than tolerating them.
 */

namespace {

inline constexpr std::uint32_t kMaxStringBytes = 4096U;

/* The v1 wire contract requires well-formed UTF-8, not merely byte strings. */
bool valid_utf8(const std::span<const std::uint8_t> text) {
    std::size_t index = 0U;
    while (index < text.size()) {
        const std::uint8_t leading = text[index++];
        if (leading <= 0x7fU) {
            continue;
        }

        std::uint32_t codepoint = 0U;
        std::uint32_t continuation_count = 0U;
        if ((leading & 0xe0U) == 0xc0U) {
            codepoint = leading & 0x1fU;
            continuation_count = 1U;
            if (codepoint < 2U) {
                return false;
            }
        } else if ((leading & 0xf0U) == 0xe0U) {
            codepoint = leading & 0x0fU;
            continuation_count = 2U;
        } else if ((leading & 0xf8U) == 0xf0U) {
            codepoint = leading & 0x07U;
            continuation_count = 3U;
        } else {
            return false;
        }

        if (index + continuation_count > text.size()) {
            return false;
        }
        for (std::uint32_t continuation = 0U; continuation < continuation_count;
             ++continuation) {
            const std::uint8_t next = text[index++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((continuation_count == 2U && codepoint < 0x800U) ||
            (continuation_count == 3U && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

std::int32_t reserve(md_writer_t* const writer, const std::size_t requested) {
    if (writer == nullptr || writer->size > writer->capacity ||
        requested > writer->capacity - writer->size ||
        (requested != 0U && writer->data == nullptr)) {
        return -ENOSPC;
    }
    return 0;
}

std::int32_t take(md_reader_t* const reader, const std::size_t requested,
                  const std::uint8_t** const data) {
    if (reader == nullptr || reader->offset > reader->size ||
        requested > reader->size - reader->offset ||
        (requested != 0U && reader->data == nullptr)) {
        return -EPROTO;
    }
    if (data != nullptr) {
        *data = reader->data + reader->offset;
    }
    reader->offset += requested;
    return 0;
}

std::int32_t read_rect(md_reader_t* const reader, md_rect_t* const rect) {
    std::int32_t result = md_read_f32(reader, &rect->x);
    if (result != 0) {
        return result;
    }
    result = md_read_f32(reader, &rect->y);
    if (result != 0) {
        return result;
    }
    result = md_read_f32(reader, &rect->width);
    if (result != 0) {
        return result;
    }
    return md_read_f32(reader, &rect->height);
}

}  // namespace


/*
 * Writer primitives below encode every wire type into the caller-provided
 * buffer.  All values are little-endian; f32 uses the IEEE-754 bit pattern.
 * Writes that would overflow the buffer return -ENOSPC without partial output.
 */
void md_writer_init(md_writer_t* const writer, std::uint8_t* const data,
                    const std::size_t capacity) {
    if (writer == nullptr) {
        return;
    }
    writer->data = data;
    writer->capacity = capacity;
    writer->size = 0U;
}

std::int32_t md_write_u16(md_writer_t* const writer, const std::uint16_t value) {
    const std::int32_t result = reserve(writer, 2U);
    if (result != 0) {
        return result;
    }
    writer->data[writer->size++] = static_cast<std::uint8_t>(value & UINT16_C(0xff));
    writer->data[writer->size++] = static_cast<std::uint8_t>((value >> 8U) & UINT16_C(0xff));
    return 0;
}

std::int32_t md_write_u32(md_writer_t* const writer, const std::uint32_t value) {
    const std::int32_t result = reserve(writer, 4U);
    if (result != 0) {
        return result;
    }
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        writer->data[writer->size++] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & UINT32_C(0xff));
    }
    return 0;
}

std::int32_t md_write_i32(md_writer_t* const writer, const std::int32_t value) {
    return md_write_u32(writer, static_cast<std::uint32_t>(value));
}

std::int32_t md_write_u64(md_writer_t* const writer, const std::uint64_t value) {
    const std::int32_t result = reserve(writer, 8U);
    if (result != 0) {
        return result;
    }
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        writer->data[writer->size++] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & UINT64_C(0xff));
    }
    return 0;
}

std::int32_t md_write_f32(md_writer_t* const writer, const float value) {
    return md_write_u32(writer, std::bit_cast<std::uint32_t>(value));
}

std::int32_t md_write_bytes(md_writer_t* const writer, const void* const data,
                            const std::size_t size) {
    const std::int32_t result = reserve(writer, size);
    if (result != 0 || (size != 0U && data == nullptr)) {
        return result != 0 ? result : -ENOSPC;
    }
    if (size != 0U) {
        const std::uint8_t* const source = static_cast<const std::uint8_t*>(data);
        std::copy_n(source, size, writer->data + writer->size);
    }
    writer->size += size;
    return 0;
}

std::int32_t md_write_string(md_writer_t* const writer, const char* const value) {
    if (value == nullptr) {
        return -EINVAL;
    }
    /* Public string inputs explicitly promise NUL termination. */
    const std::string_view text(value);
    if (text.size() > kMaxStringBytes ||
        !valid_utf8(std::span<const std::uint8_t>(
            static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()))) {
        return -EINVAL;
    }
    const std::int32_t result = md_write_u32(writer, static_cast<std::uint32_t>(text.size()));
    if (result != 0) {
        return result;
    }
    return md_write_bytes(writer, text.data(), text.size());
}


/*
 * Reader primitives below consume the packet payload sequentially without
 * allocating.  Any read that would run past the end of the buffer returns
 * -EPROTO, so a malformed packet can never make a decoder read out of bounds.
 */
void md_reader_init(md_reader_t* const reader, const std::uint8_t* const data,
                    const std::size_t size) {
    if (reader == nullptr) {
        return;
    }
    reader->data = data;
    reader->size = size;
    reader->offset = 0U;
}

std::int32_t md_read_u16(md_reader_t* const reader, std::uint16_t* const value) {
    const std::uint8_t* data = nullptr;
    if (value == nullptr || take(reader, 2U, &data) != 0) {
        return -EPROTO;
    }
    *value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                        (static_cast<std::uint16_t>(data[1]) << 8U));
    return 0;
}

std::int32_t md_read_u32(md_reader_t* const reader, std::uint32_t* const value) {
    const std::uint8_t* data = nullptr;
    if (value == nullptr || take(reader, 4U, &data) != 0) {
        return -EPROTO;
    }
    *value = static_cast<std::uint32_t>(data[0]) |
             (static_cast<std::uint32_t>(data[1]) << 8U) |
             (static_cast<std::uint32_t>(data[2]) << 16U) |
             (static_cast<std::uint32_t>(data[3]) << 24U);
    return 0;
}

std::int32_t md_read_i32(md_reader_t* const reader, std::int32_t* const value) {
    std::uint32_t bits = 0U;
    const std::int32_t result = md_read_u32(reader, &bits);
    if (value == nullptr || result != 0) return -EPROTO;
    *value = static_cast<std::int32_t>(bits);
    return 0;
}

std::int32_t md_read_u64(md_reader_t* const reader, std::uint64_t* const value) {
    const std::uint8_t* data = nullptr;
    if (value == nullptr || take(reader, 8U, &data) != 0) {
        return -EPROTO;
    }
    std::uint64_t decoded = 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        decoded |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
    }
    *value = decoded;
    return 0;
}

std::int32_t md_read_f32(md_reader_t* const reader, float* const value) {
    std::uint32_t bits = 0U;
    const std::int32_t result = md_read_u32(reader, &bits);
    if (value == nullptr || result != 0) {
        return -EPROTO;
    }
    *value = std::bit_cast<float>(bits);
    return 0;
}

std::int32_t md_read_bytes(md_reader_t* const reader, void* const data,
                           const std::size_t size) {
    const std::uint8_t* source = nullptr;
    if ((size != 0U && data == nullptr) || take(reader, size, &source) != 0) {
        return -EPROTO;
    }
    if (size != 0U) {
        std::memcpy(data, source, size);
    }
    return 0;
}

std::int32_t md_read_string(md_reader_t* const reader, char** const value) {
    std::uint32_t length = 0U;
    const std::uint8_t* data = nullptr;
    if (value == nullptr || md_read_u32(reader, &length) != 0 || length > kMaxStringBytes ||
        take(reader, length, &data) != 0 ||
        !valid_utf8(std::span<const std::uint8_t>(data, length))) {
        return -EPROTO;
    }

    std::unique_ptr<char[]> decoded(new (std::nothrow) char[static_cast<std::size_t>(length) + 1U]);
    if (decoded == nullptr) {
        return -ENOMEM;
    }
    std::copy_n(data, length, decoded.get());
    decoded[length] = '\0';
    *value = decoded.release();
    return 0;
}

void md_protocol_free_string(char* const value) {
    delete[] value;
}

std::int32_t md_reader_finish(const md_reader_t* const reader) {
    return reader != nullptr && reader->offset == reader->size ? 0 : -EPROTO;
}


/*
 * Per-message encoders below mirror the XML protocol definition field for
 * field; the golden-vector tests pin each order and width.  Registration and caps
 * messages take borrowed caller structs, while input messages take their
 * individual values.
 */
std::int32_t md_proto_encode_hello(md_writer_t* const writer, const std::uint32_t role,
                                   const char* const name, const char* const version,
                                   const std::uint64_t features) {
    if (role != 1U && role != 2U) {
        return -EINVAL;
    }
    std::int32_t result = md_write_u32(writer, role);
    if (result != 0) return result;
    /* v1.1 target-GPU binding is mandatory for project endpoints. Do not
     * negotiate v1.0, whose OUTPUT_CONFIG has no consumer GPU identity. */
    result = md_write_u16(writer, 0U);
    if (result != 0) return result;
    result = md_write_u16(writer, MIRAGE_DISPLAY_PROTOCOL_MINOR);
    if (result != 0) return result;
    result = md_write_u16(writer, MIRAGE_DISPLAY_PROTOCOL_MINOR);
    if (result != 0) return result;
    result = md_write_u64(writer, features);
    if (result != 0) return result;
    result = md_write_string(writer, name);
    if (result != 0) return result;
    return md_write_string(writer, version);
}

std::int32_t md_proto_encode_register_output(md_writer_t* const writer,
                                             const md_output_info_t* const output) {
    if (output == nullptr || output->stable_id == nullptr || output->name == nullptr) {
        return -EINVAL;
    }
    std::int32_t result = md_write_string(writer, output->stable_id);
    if (result != 0) return result;
    result = md_write_string(writer, output->name);
    if (result != 0) return result;
    result = md_write_u32(writer, output->physical_width);
    if (result != 0) return result;
    result = md_write_u32(writer, output->physical_height);
    if (result != 0) return result;
    result = md_write_u32(writer, output->logical_width);
    if (result != 0) return result;
    result = md_write_u32(writer, output->logical_height);
    if (result != 0) return result;
    result = md_write_i32(writer, output->logical_x);
    if (result != 0) return result;
    result = md_write_i32(writer, output->logical_y);
    if (result != 0) return result;
    result = md_write_u32(writer, output->scale_120);
    if (result != 0) return result;
    result = md_write_u32(writer, output->refresh_mhz);
    if (result != 0) return result;
    result = md_write_u32(writer, static_cast<std::uint32_t>(output->transform));
    if (result != 0) return result;
    result = md_write_u32(writer, output->drm_render_major);
    if (result != 0) return result;
    result = md_write_u32(writer, output->drm_render_minor);
    if (result != 0) return result;
    return md_write_u64(writer, output->input_caps);
}

std::int32_t md_proto_encode_update_output(md_writer_t* const writer,
                                           const md_output_info_t* const output) {
    if (output == nullptr) {
        return -EINVAL;
    }
    std::int32_t result = md_write_u32(writer, output->physical_width);
    if (result != 0) return result;
    result = md_write_u32(writer, output->physical_height);
    if (result != 0) return result;
    result = md_write_u32(writer, output->logical_width);
    if (result != 0) return result;
    result = md_write_u32(writer, output->logical_height);
    if (result != 0) return result;
    result = md_write_i32(writer, output->logical_x);
    if (result != 0) return result;
    result = md_write_i32(writer, output->logical_y);
    if (result != 0) return result;
    result = md_write_u32(writer, output->scale_120);
    if (result != 0) return result;
    result = md_write_u32(writer, output->refresh_mhz);
    if (result != 0) return result;
    return md_write_u32(writer, static_cast<std::uint32_t>(output->transform));
}

std::int32_t md_proto_encode_consumer_caps(md_writer_t* const writer,
                                           const md_consumer_caps_t* const caps) {
    if (caps == nullptr || caps->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        (caps->format_count != 0U && caps->formats == nullptr)) {
        return -EINVAL;
    }
    std::int32_t result = md_write_u64(writer, caps->sync_caps);
    if (result != 0) return result;
    result = md_write_u64(writer, caps->color_caps);
    if (result != 0) return result;
    result = md_write_u32(writer, caps->max_width);
    if (result != 0) return result;
    result = md_write_u32(writer, caps->max_height);
    if (result != 0) return result;
    result = md_write_bytes(writer, caps->device_uuid, sizeof(caps->device_uuid));
    if (result != 0) return result;
    result = md_write_bytes(writer, caps->driver_uuid, sizeof(caps->driver_uuid));
    if (result != 0) return result;
    result = md_write_u32(writer, caps->format_count);
    if (result != 0) return result;
    for (std::uint32_t index = 0U; index < caps->format_count; ++index) {
        result = md_write_u32(writer, caps->formats[index].fourcc);
        if (result != 0) return result;
        result = md_write_u32(writer, caps->formats[index].plane_count);
        if (result != 0) return result;
        result = md_write_u64(writer, caps->formats[index].modifier);
        if (result != 0) return result;
    }
    return 0;
}

std::int32_t md_proto_encode_pointer_enter(md_writer_t* const writer, const float x,
                                           const float y, const std::uint64_t timestamp_us) {
    std::int32_t result = md_write_f32(writer, x);
    if (result != 0) return result;
    result = md_write_f32(writer, y);
    if (result != 0) return result;
    return md_write_u64(writer, timestamp_us);
}

std::int32_t md_proto_encode_pointer_leave(md_writer_t* const writer,
                                           const std::uint64_t timestamp_us) {
    return md_write_u64(writer, timestamp_us);
}

std::int32_t md_proto_encode_pointer_motion(md_writer_t* const writer, const float x,
                                            const float y, const std::uint64_t timestamp_us,
                                            const std::uint32_t modifiers) {
    std::int32_t result = md_write_f32(writer, x);
    if (result != 0) return result;
    result = md_write_f32(writer, y);
    if (result != 0) return result;
    result = md_write_u64(writer, timestamp_us);
    if (result != 0) return result;
    return md_write_u32(writer, modifiers);
}

std::int32_t md_proto_encode_pointer_button(md_writer_t* const writer, const float x,
                                            const float y, const std::uint32_t button,
                                            const md_button_state_t state,
                                            const std::uint64_t timestamp_us,
                                            const std::uint32_t modifiers) {
    std::int32_t result = md_write_f32(writer, x);
    if (result != 0) return result;
    result = md_write_f32(writer, y);
    if (result != 0) return result;
    result = md_write_u32(writer, button);
    if (result != 0) return result;
    result = md_write_u32(writer, static_cast<std::uint32_t>(state));
    if (result != 0) return result;
    result = md_write_u64(writer, timestamp_us);
    if (result != 0) return result;
    return md_write_u32(writer, modifiers);
}

std::int32_t md_proto_encode_pointer_axis(md_writer_t* const writer, const float x,
                                          const float y, const float delta_x,
                                          const float delta_y, const md_axis_source_t source,
                                          const std::uint64_t timestamp_us,
                                          const std::uint32_t modifiers) {
    std::int32_t result = md_write_f32(writer, x);
    if (result != 0) return result;
    result = md_write_f32(writer, y);
    if (result != 0) return result;
    result = md_write_f32(writer, delta_x);
    if (result != 0) return result;
    result = md_write_f32(writer, delta_y);
    if (result != 0) return result;
    result = md_write_u32(writer, static_cast<std::uint32_t>(source));
    if (result != 0) return result;
    result = md_write_u64(writer, timestamp_us);
    if (result != 0) return result;
    return md_write_u32(writer, modifiers);
}

std::int32_t md_proto_encode_u32(md_writer_t* const writer, const std::uint32_t value) {
    return md_write_u32(writer, value);
}

std::int32_t md_proto_encode_u64(md_writer_t* const writer, const std::uint64_t value) {
    return md_write_u64(writer, value);
}

std::int32_t md_proto_encode_register_producer(md_writer_t* const writer,
                                               const md_producer_info_t* const info) {
    if (info == nullptr || info->stable_output_id == nullptr || info->kind == nullptr ||
        info->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        (info->format_count != 0U && info->formats == nullptr)) {
        return -EINVAL;
    }
    std::int32_t result = md_write_string(writer, info->stable_output_id);
    if (result != 0) return result;
    result = md_write_string(writer, info->kind);
    if (result != 0) return result;
    result = md_write_u32(writer, info->drm_render_major);
    if (result != 0) return result;
    result = md_write_u32(writer, info->drm_render_minor);
    if (result != 0) return result;
    result = md_write_bytes(writer, info->device_uuid, sizeof(info->device_uuid));
    if (result != 0) return result;
    result = md_write_bytes(writer, info->driver_uuid, sizeof(info->driver_uuid));
    if (result != 0) return result;
    result = md_write_u32(writer, info->format_count);
    if (result != 0) return result;
    for (std::uint32_t index = 0U; index < info->format_count; ++index) {
        if (info->formats[index].plane_count == 0U ||
            info->formats[index].plane_count > MIRAGE_DISPLAY_MAX_PLANES) {
            return -EINVAL;
        }
        result = md_write_u32(writer, info->formats[index].fourcc);
        if (result != 0) return result;
        result = md_write_u32(writer, info->formats[index].plane_count);
        if (result != 0) return result;
        result = md_write_u64(writer, info->formats[index].modifier);
        if (result != 0) return result;
    }
    return 0;
}

std::int32_t md_proto_encode_producer_gpu_bound(
    md_writer_t* const writer, const md_producer_gpu_info_t* const gpu) {
    if (gpu == nullptr || gpu->drm_render_minor < 128U || gpu->drm_render_minor > 255U) {
        return -EINVAL;
    }
    std::int32_t result = md_write_u32(writer, gpu->drm_render_major);
    if (result == 0) result = md_write_u32(writer, gpu->drm_render_minor);
    if (result == 0) result = md_write_bytes(writer, gpu->device_uuid, sizeof(gpu->device_uuid));
    if (result == 0) result = md_write_bytes(writer, gpu->driver_uuid, sizeof(gpu->driver_uuid));
    return result;
}

std::int32_t md_proto_encode_offer_buffers(md_writer_t* const writer,
                                           const md_buffer_pool_t* const pool) {
    if (pool == nullptr || pool->buffer_count < 2U ||
        pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS || pool->plane_count == 0U ||
        pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES || pool->generation == 0U) {
        return -EINVAL;
    }
    const std::uint32_t descriptor_count = pool->buffer_count * pool->plane_count;
    std::int32_t result = md_write_u64(writer, pool->generation);
    if (result != 0) return result;
    result = md_write_u32(writer, pool->buffer_count);
    if (result != 0) return result;
    result = md_write_u32(writer, pool->width);
    if (result != 0) return result;
    result = md_write_u32(writer, pool->height);
    if (result != 0) return result;
    result = md_write_u32(writer, pool->fourcc);
    if (result != 0) return result;
    result = md_write_u32(writer, pool->plane_count);
    if (result != 0) return result;
    result = md_write_u64(writer, pool->modifier);
    if (result != 0) return result;
    result = md_write_u32(writer, descriptor_count);
    if (result != 0) return result;
    for (std::uint32_t buffer = 0U; buffer < pool->buffer_count; ++buffer) {
        for (std::uint32_t plane = 0U; plane < pool->plane_count; ++plane) {
            result = md_write_u32(writer, pool->planes[buffer][plane].stride);
            if (result != 0) return result;
            result = md_write_u32(writer, pool->planes[buffer][plane].offset);
            if (result != 0) return result;
            result = md_write_u64(writer, pool->planes[buffer][plane].size);
            if (result != 0) return result;
        }
    }
    return 0;
}

std::int32_t md_proto_encode_producer_frame(md_writer_t* const writer,
                                            const std::uint64_t generation,
                                            const std::uint32_t buffer_index,
                                            const std::uint64_t sequence) {
    if (generation == 0U) {
        return -EINVAL;
    }
    std::int32_t result = md_write_u64(writer, generation);
    if (result != 0) return result;
    result = md_write_u32(writer, buffer_index);
    if (result != 0) return result;
    result = md_write_u32(writer, 0U);
    if (result != 0) return result;
    return md_write_u64(writer, sequence);
}

std::int32_t md_proto_encode_config(md_writer_t* const writer,
                                    const md_display_config_t* const config) {
    if (config == nullptr || config->generation == 0U ||
        config->transform > MD_TRANSFORM_FLIPPED_270) {
        return -EINVAL;
    }
    std::int32_t result = md_write_u64(writer, config->generation);
    if (result != 0) return result;
    result = md_write_f32(writer, config->source.x);
    if (result != 0) return result;
    result = md_write_f32(writer, config->source.y);
    if (result != 0) return result;
    result = md_write_f32(writer, config->source.width);
    if (result != 0) return result;
    result = md_write_f32(writer, config->source.height);
    if (result != 0) return result;
    result = md_write_f32(writer, config->destination.x);
    if (result != 0) return result;
    result = md_write_f32(writer, config->destination.y);
    if (result != 0) return result;
    result = md_write_f32(writer, config->destination.width);
    if (result != 0) return result;
    result = md_write_f32(writer, config->destination.height);
    if (result != 0) return result;
    result = md_write_u32(writer, static_cast<std::uint32_t>(config->transform));
    if (result != 0) return result;
    for (std::size_t index = 0U; index < 4U; ++index) {
        result = md_write_f32(writer, config->clear_color[index]);
        if (result != 0) return result;
    }
    return 0;
}


/*
 * Per-message decoders below allocate only for variable-length strings (and
 * always pair with the matching md_proto_*_clear helper).  A decoder that fails
 * midway reports -EPROTO; callers treat that as a fatal protocol error.
 */
std::int32_t md_proto_decode_welcome(const std::uint8_t* const data, const std::size_t size,
                                     md_proto_welcome_t* const welcome) {
    if (welcome == nullptr) {
        return -EINVAL;
    }
    welcome->selected_minor = 0U;
    welcome->features = 0U;
    welcome->server_name = nullptr;
    welcome->server_version = nullptr;
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint16_t reserved = 0U;
    std::int32_t result = md_read_u16(&reader, &welcome->selected_minor);
    if (result != 0) {
        return result;
    }
    result = md_read_u16(&reader, &reserved);
    if (result != 0 || reserved != 0U) {
        return result != 0 ? result : -EPROTO;
    }
    result = md_read_u64(&reader, &welcome->features);
    if (result != 0) {
        return result;
    }
    result = md_read_string(&reader, &welcome->server_name);
    if (result == 0) {
        result = md_read_string(&reader, &welcome->server_version);
    }
    if (result == 0) {
        result = md_reader_finish(&reader);
    }
    if (result != 0) {
        md_proto_welcome_clear(welcome);
    }
    return result;
}

void md_proto_welcome_clear(md_proto_welcome_t* const welcome) {
    if (welcome == nullptr) {
        return;
    }
    md_protocol_free_string(welcome->server_name);
    md_protocol_free_string(welcome->server_version);
    welcome->selected_minor = 0U;
    welcome->features = 0U;
    welcome->server_name = nullptr;
    welcome->server_version = nullptr;
}

std::int32_t md_proto_decode_error(const std::uint8_t* const data, const std::size_t size,
                                   md_proto_error_t* const error) {
    if (error == nullptr) {
        return -EINVAL;
    }
    error->code = 0U;
    error->fatal = UINT8_C(0);
    error->message = nullptr;
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t fatal = 0U;
    std::int32_t result = md_read_u32(&reader, &error->code);
    if (result == 0) {
        result = md_read_u32(&reader, &fatal);
    }
    if (result == 0) {
        error->fatal = fatal == 0U ? UINT8_C(0) : UINT8_C(1);
        result = md_read_string(&reader, &error->message);
    }
    if (result == 0) {
        result = md_reader_finish(&reader);
    }
    if (result != 0) {
        md_proto_error_clear(error);
    }
    return result;
}

void md_proto_error_clear(md_proto_error_t* const error) {
    if (error == nullptr) {
        return;
    }
    md_protocol_free_string(error->message);
    error->code = 0U;
    error->fatal = UINT8_C(0);
    error->message = nullptr;
}

std::int32_t md_proto_decode_output_accepted(const std::uint8_t* const data,
                                             const std::size_t size,
                                             std::uint64_t* const output_id) {
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    const std::int32_t result = md_read_u64(&reader, output_id);
    return result == 0 ? md_reader_finish(&reader) : result;
}

std::int32_t md_proto_decode_bind_buffers(const std::uint8_t* const data,
                                          const std::size_t size,
                                          md_buffer_pool_t* const pool) {
    if (pool == nullptr) {
        return -EINVAL;
    }
    md_init_pool(pool);
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t descriptor_count = 0U;
    std::int32_t result = md_read_u64(&reader, &pool->generation);
    if (result == 0) result = md_read_u32(&reader, &pool->buffer_count);
    if (result == 0) result = md_read_u32(&reader, &pool->width);
    if (result == 0) result = md_read_u32(&reader, &pool->height);
    if (result == 0) result = md_read_u32(&reader, &pool->fourcc);
    if (result == 0) result = md_read_u32(&reader, &pool->plane_count);
    if (result == 0) result = md_read_u64(&reader, &pool->modifier);
    if (result == 0) result = md_read_u32(&reader, &descriptor_count);
    if (result != 0 || pool->buffer_count < 2U ||
        pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS || pool->plane_count == 0U ||
        pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        descriptor_count != pool->buffer_count * pool->plane_count) {
        return result != 0 ? result : -EPROTO;
    }
    for (std::uint32_t buffer = 0U; buffer < pool->buffer_count; ++buffer) {
        for (std::uint32_t plane = 0U; plane < pool->plane_count; ++plane) {
            result = md_read_u32(&reader, &pool->planes[buffer][plane].stride);
            if (result != 0) return result;
            result = md_read_u32(&reader, &pool->planes[buffer][plane].offset);
            if (result != 0) return result;
            result = md_read_u64(&reader, &pool->planes[buffer][plane].size);
            if (result != 0) return result;
        }
    }
    return md_reader_finish(&reader);
}

std::int32_t md_proto_decode_config(const std::uint8_t* const data, const std::size_t size,
                                    md_display_config_t* const config) {
    if (config == nullptr) {
        return -EINVAL;
    }
    std::memset(config, 0, sizeof(*config));
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t transform = 0U;
    std::int32_t result = md_read_u64(&reader, &config->generation);
    if (result == 0) result = read_rect(&reader, &config->source);
    if (result == 0) result = read_rect(&reader, &config->destination);
    if (result == 0) result = md_read_u32(&reader, &transform);
    if (result != 0 || transform > MD_TRANSFORM_FLIPPED_270) {
        return result != 0 ? result : -EPROTO;
    }
    config->transform = static_cast<md_transform_t>(transform);
    for (std::size_t index = 0U; index < 4U; ++index) {
        result = md_read_f32(&reader, &config->clear_color[index]);
        if (result != 0) return result;
    }
    return md_reader_finish(&reader);
}

std::int32_t md_proto_decode_frame(const std::uint8_t* const data, const std::size_t size,
                                   md_frame_t* const frame) {
    if (frame == nullptr) {
        return -EINVAL;
    }
    frame->buffer_generation = 0U;
    frame->buffer_index = 0U;
    frame->sequence = 0U;
    frame->acquire_sync_fd = mirage::kInvalidFd;
    frame->release_syncobj_fd = mirage::kInvalidFd;
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t reserved = 0U;
    std::int32_t result = md_read_u64(&reader, &frame->buffer_generation);
    if (result == 0) result = md_read_u32(&reader, &frame->buffer_index);
    if (result == 0) result = md_read_u32(&reader, &reserved);
    if (result != 0 || reserved != 0U) {
        return result != 0 ? result : -EPROTO;
    }
    result = md_read_u64(&reader, &frame->sequence);
    return result == 0 ? md_reader_finish(&reader) : result;
}

std::int32_t md_proto_decode_unbind(const std::uint8_t* const data, const std::size_t size,
                                    std::uint64_t* const generation) {
    return md_proto_decode_output_accepted(data, size, generation);
}

std::int32_t md_proto_decode_producer_accepted(const std::uint8_t* const data,
                                               const std::size_t size,
                                               std::uint64_t* const producer_id,
                                               std::uint64_t* const output_id) {
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::int32_t result = md_read_u64(&reader, producer_id);
    if (result == 0) result = md_read_u64(&reader, output_id);
    return result == 0 ? md_reader_finish(&reader) : result;
}

std::int32_t md_proto_decode_output_config(const std::uint8_t* const data,
                                           const std::size_t size,
                                           md_producer_config_t* const config) {
    if (config == nullptr) {
        return -EINVAL;
    }
    std::memset(config, 0, sizeof(*config));
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t transform = 0U;
    std::int32_t result = md_read_u32(&reader, &config->physical_width);
    if (result == 0) result = md_read_u32(&reader, &config->physical_height);
    if (result == 0) result = md_read_u32(&reader, &config->logical_width);
    if (result == 0) result = md_read_u32(&reader, &config->logical_height);
    if (result == 0) result = md_read_u32(&reader, &config->refresh_mhz);
    if (result == 0) result = md_read_u32(&reader, &transform);
    if (result == 0) result = md_read_u32(&reader, &config->fourcc);
    if (result == 0) result = md_read_u32(&reader, &config->plane_count);
    if (result == 0) result = md_read_u64(&reader, &config->modifier);
    if (result == 0) result = md_read_u32(&reader, &config->target_drm_render_major);
    if (result == 0) result = md_read_u32(&reader, &config->target_drm_render_minor);
    if (result == 0) result = md_read_u32(&reader, &config->target_gpu_flags);
    if (result == 0) result = md_read_bytes(&reader, config->target_device_uuid,
                                            sizeof(config->target_device_uuid));
    if (result == 0) result = md_read_bytes(&reader, config->target_driver_uuid,
                                            sizeof(config->target_driver_uuid));
    if (result != 0 || config->physical_width == 0U || config->physical_height == 0U ||
        config->logical_width == 0U || config->logical_height == 0U ||
        transform > MD_TRANSFORM_FLIPPED_270 || config->plane_count == 0U ||
        config->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        (config->target_gpu_flags & MD_TARGET_GPU_RENDER_NODE_VALID) == 0U ||
        config->target_drm_render_minor < 128U || config->target_drm_render_minor > 255U) {
        return result != 0 ? result : -EPROTO;
    }
    config->transform = static_cast<md_transform_t>(transform);
    return md_reader_finish(&reader);
}

std::int32_t md_proto_decode_producer_gpu_bound(
    const std::uint8_t* const data, const std::size_t size,
    md_producer_gpu_info_t* const gpu) {
    if (gpu == nullptr) return -EINVAL;
    std::memset(gpu, 0, sizeof(*gpu));
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::int32_t result = md_read_u32(&reader, &gpu->drm_render_major);
    if (result == 0) result = md_read_u32(&reader, &gpu->drm_render_minor);
    if (result == 0) result = md_read_bytes(&reader, gpu->device_uuid, sizeof(gpu->device_uuid));
    if (result == 0) result = md_read_bytes(&reader, gpu->driver_uuid, sizeof(gpu->driver_uuid));
    if (result != 0 || gpu->drm_render_minor < 128U || gpu->drm_render_minor > 255U) {
        return result != 0 ? result : -EPROTO;
    }
    return md_reader_finish(&reader);
}

std::int32_t md_proto_decode_pointer_enter(const std::uint8_t* const data,
                                           const std::size_t size,
                                           md_pointer_enter_t* const event) {
    if (event == nullptr) {
        return -EINVAL;
    }
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::int32_t result = md_read_f32(&reader, &event->x);
    if (result == 0) result = md_read_f32(&reader, &event->y);
    if (result == 0) result = md_read_u64(&reader, &event->timestamp_us);
    return result == 0 ? md_reader_finish(&reader) : result;
}

std::int32_t md_proto_decode_pointer_leave(const std::uint8_t* const data,
                                           const std::size_t size,
                                           std::uint64_t* const timestamp_us) {
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    const std::int32_t result = md_read_u64(&reader, timestamp_us);
    return result == 0 ? md_reader_finish(&reader) : result;
}

std::int32_t md_proto_decode_pointer_motion(const std::uint8_t* const data,
                                            const std::size_t size,
                                            md_pointer_motion_t* const event) {
    if (event == nullptr) {
        return -EINVAL;
    }
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::int32_t result = md_read_f32(&reader, &event->x);
    if (result == 0) result = md_read_f32(&reader, &event->y);
    if (result == 0) result = md_read_u64(&reader, &event->timestamp_us);
    if (result == 0) result = md_read_u32(&reader, &event->modifiers);
    return result == 0 ? md_reader_finish(&reader) : result;
}

std::int32_t md_proto_decode_pointer_button(const std::uint8_t* const data,
                                            const std::size_t size,
                                            md_pointer_button_t* const event) {
    if (event == nullptr) {
        return -EINVAL;
    }
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t state = 0U;
    std::int32_t result = md_read_f32(&reader, &event->x);
    if (result == 0) result = md_read_f32(&reader, &event->y);
    if (result == 0) result = md_read_u32(&reader, &event->button);
    if (result == 0) result = md_read_u32(&reader, &state);
    if (result == 0) result = md_read_u64(&reader, &event->timestamp_us);
    if (result == 0) result = md_read_u32(&reader, &event->modifiers);
    if (result != 0 || state > MD_BUTTON_PRESSED) {
        return result != 0 ? result : -EPROTO;
    }
    event->state = static_cast<md_button_state_t>(state);
    return md_reader_finish(&reader);
}

std::int32_t md_proto_decode_pointer_axis(const std::uint8_t* const data,
                                          const std::size_t size,
                                          md_pointer_axis_t* const event) {
    if (event == nullptr) {
        return -EINVAL;
    }
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::uint32_t source = 0U;
    std::int32_t result = md_read_f32(&reader, &event->x);
    if (result == 0) result = md_read_f32(&reader, &event->y);
    if (result == 0) result = md_read_f32(&reader, &event->delta_x);
    if (result == 0) result = md_read_f32(&reader, &event->delta_y);
    if (result == 0) result = md_read_u32(&reader, &source);
    if (result == 0) result = md_read_u64(&reader, &event->timestamp_us);
    if (result == 0) result = md_read_u32(&reader, &event->modifiers);
    if (result != 0 || source > MD_AXIS_CONTINUOUS) {
        return result != 0 ? result : -EPROTO;
    }
    event->source = static_cast<md_axis_source_t>(source);
    return md_reader_finish(&reader);
}

std::int32_t md_proto_decode_window_state(const std::uint8_t* const data,
                                          const std::size_t size,
                                          std::uint32_t* const flags) {
    if (flags == nullptr) {
        return -EINVAL;
    }
    md_reader_t reader;
    md_reader_init(&reader, data, size);
    std::int32_t result = md_read_u32(&reader, flags);
    // The WINDOW_STATE payload is exactly one u32; md_reader_finish rejects
    // trailing bytes so a malformed longer payload cannot be silently accepted.
    return result == 0 ? md_reader_finish(&reader) : result;
}
