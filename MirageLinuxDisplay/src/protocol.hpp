#ifndef MIRAGE_DISPLAY_PROTOCOL_HPP
#define MIRAGE_DISPLAY_PROTOCOL_HPP

#include "mirage_display.h"
#include "mirage_display_producer.h"

#include <cstddef>
#include <cstdint>

/*
 * mirage-display-v1 opcodes, wire primitives, and per-message encoders/decoders.
 *
 * Encoders write into a caller-provided buffer; decoders consume bytes
 * sequentially without allocating.  Every decoder enforces the documented wire
 * limits (strings <= 4096 bytes, exact element counts, no trailing bytes) and
 * reports MD_ERR_PROTOCOL instead of guessing at malformed input.
 */

enum md_opcode : std::uint16_t {
    MD_OP_HELLO = 0x0001,
    MD_OP_GOODBYE = 0x0002,

    MD_OP_REGISTER_OUTPUT = 0x0101,
    MD_OP_UPDATE_OUTPUT = 0x0102,
    MD_OP_CONSUMER_CAPS = 0x0103,
    MD_OP_POINTER_ENTER = 0x0104,
    MD_OP_POINTER_LEAVE = 0x0105,
    MD_OP_POINTER_MOTION = 0x0106,
    MD_OP_POINTER_BUTTON = 0x0107,
    MD_OP_POINTER_AXIS = 0x0108,
    MD_OP_UNBIND_DONE = 0x0109,
    MD_OP_WINDOW_STATE = 0x010a,

    MD_OP_REGISTER_PRODUCER = 0x0201,
    MD_OP_OFFER_BUFFERS = 0x0202,
    MD_OP_PRODUCER_FRAME = 0x0203,
    MD_OP_RETIRE_DONE = 0x0204,
    MD_OP_PRODUCER_SET_CONFIG = 0x0205,
    MD_OP_PRODUCER_GPU_BOUND = 0x0206,

    MD_OP_WELCOME = 0x8001,
    MD_OP_ERROR = 0x80ff,
    MD_OP_OUTPUT_ACCEPTED = 0x8101,
    MD_OP_BIND_BUFFERS = 0x8102,
    MD_OP_SET_CONFIG = 0x8103,
    MD_OP_FRAME_READY = 0x8104,
    MD_OP_UNBIND = 0x8105,

    MD_OP_PRODUCER_ACCEPTED = 0x8201,
    MD_OP_OUTPUT_CONFIG = 0x8202,
    MD_OP_RETIRE_BUFFERS = 0x8203,
    MD_OP_PRODUCER_POINTER_MOTION = 0x8204,
    MD_OP_PRODUCER_POINTER_BUTTON = 0x8205,
    MD_OP_PRODUCER_POINTER_ENTER = 0x8206,
    MD_OP_PRODUCER_POINTER_LEAVE = 0x8207,
    MD_OP_PRODUCER_POINTER_AXIS = 0x8208,
    MD_OP_PRODUCER_WINDOW_STATE = 0x8209,
};

/* Borrowed caller-owned storage for encoding a single protocol payload. */
struct md_writer_t {
    std::uint8_t* data;
    std::size_t capacity;
    std::size_t size;
};

/* Borrowed packet bytes decoded sequentially without allocating. */
struct md_reader_t {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset;
};

/* Text fields are allocated by the decoder and released with md_protocol_free_string(). */
struct md_proto_welcome_t {
    std::uint16_t selected_minor;
    std::uint64_t features;
    char* server_name;
    char* server_version;
};

/* message is decoder-owned and released with md_proto_error_clear(). */
struct md_proto_error_t {
    std::uint32_t code;
    std::uint8_t fatal;
    char* message;
};

void md_writer_init(md_writer_t* writer, std::uint8_t* data, std::size_t capacity);
std::int32_t md_write_u16(md_writer_t* writer, std::uint16_t value);
std::int32_t md_write_u32(md_writer_t* writer, std::uint32_t value);
std::int32_t md_write_i32(md_writer_t* writer, std::int32_t value);
std::int32_t md_write_u64(md_writer_t* writer, std::uint64_t value);
std::int32_t md_write_f32(md_writer_t* writer, float value);
std::int32_t md_write_bytes(md_writer_t* writer, const void* data, std::size_t size);
std::int32_t md_write_string(md_writer_t* writer, const char* value);

void md_reader_init(md_reader_t* reader, const std::uint8_t* data, std::size_t size);
std::int32_t md_read_u16(md_reader_t* reader, std::uint16_t* value);
std::int32_t md_read_u32(md_reader_t* reader, std::uint32_t* value);
std::int32_t md_read_i32(md_reader_t* reader, std::int32_t* value);
std::int32_t md_read_u64(md_reader_t* reader, std::uint64_t* value);
std::int32_t md_read_f32(md_reader_t* reader, float* value);
std::int32_t md_read_bytes(md_reader_t* reader, void* data, std::size_t size);
/* Allocates a NUL-terminated UTF-8 string; caller transfers it to md_protocol_free_string(). */
std::int32_t md_read_string(md_reader_t* reader, char** value);
void md_protocol_free_string(char* value);
std::int32_t md_reader_finish(const md_reader_t* reader);

std::int32_t md_proto_encode_hello(md_writer_t* writer, std::uint32_t role,
                                   const char* name, const char* version,
                                   std::uint64_t features);
std::int32_t md_proto_encode_register_output(md_writer_t* writer,
                                             const md_output_info_t* output);
std::int32_t md_proto_encode_update_output(md_writer_t* writer,
                                           const md_output_info_t* output);
std::int32_t md_proto_encode_consumer_caps(md_writer_t* writer,
                                           const md_consumer_caps_t* caps);
std::int32_t md_proto_encode_pointer_enter(md_writer_t* writer, float x, float y,
                                           std::uint64_t timestamp_us);
std::int32_t md_proto_encode_pointer_leave(md_writer_t* writer, std::uint64_t timestamp_us);
std::int32_t md_proto_encode_pointer_motion(md_writer_t* writer, float x, float y,
                                            std::uint64_t timestamp_us,
                                            std::uint32_t modifiers);
std::int32_t md_proto_encode_pointer_button(md_writer_t* writer, float x, float y,
                                            std::uint32_t button, md_button_state_t state,
                                            std::uint64_t timestamp_us,
                                            std::uint32_t modifiers);
std::int32_t md_proto_encode_pointer_axis(md_writer_t* writer, float x, float y,
                                          float delta_x, float delta_y,
                                          md_axis_source_t source,
                                          std::uint64_t timestamp_us,
                                          std::uint32_t modifiers);
std::int32_t md_proto_encode_u32(md_writer_t* writer, std::uint32_t value);
std::int32_t md_proto_encode_u64(md_writer_t* writer, std::uint64_t value);
std::int32_t md_proto_encode_register_producer(md_writer_t* writer,
                                               const md_producer_info_t* info);
std::int32_t md_proto_encode_producer_gpu_bound(md_writer_t* writer,
                                                const md_producer_gpu_info_t* gpu);
std::int32_t md_proto_encode_offer_buffers(md_writer_t* writer,
                                           const md_buffer_pool_t* pool);
std::int32_t md_proto_encode_producer_frame(md_writer_t* writer,
                                            std::uint64_t generation,
                                            std::uint32_t buffer_index,
                                            std::uint64_t sequence);
std::int32_t md_proto_encode_config(md_writer_t* writer, const md_display_config_t* config);

std::int32_t md_proto_decode_welcome(const std::uint8_t* data, std::size_t size,
                                     md_proto_welcome_t* welcome);
void md_proto_welcome_clear(md_proto_welcome_t* welcome);
std::int32_t md_proto_decode_error(const std::uint8_t* data, std::size_t size,
                                   md_proto_error_t* error);
void md_proto_error_clear(md_proto_error_t* error);
std::int32_t md_proto_decode_output_accepted(const std::uint8_t* data, std::size_t size,
                                             std::uint64_t* output_id);
std::int32_t md_proto_decode_bind_buffers(const std::uint8_t* data, std::size_t size,
                                          md_buffer_pool_t* pool);
std::int32_t md_proto_decode_config(const std::uint8_t* data, std::size_t size,
                                    md_display_config_t* config);
std::int32_t md_proto_decode_frame(const std::uint8_t* data, std::size_t size,
                                   md_frame_t* frame);
std::int32_t md_proto_decode_unbind(const std::uint8_t* data, std::size_t size,
                                    std::uint64_t* generation);
std::int32_t md_proto_decode_producer_accepted(const std::uint8_t* data, std::size_t size,
                                               std::uint64_t* producer_id,
                                               std::uint64_t* output_id);
std::int32_t md_proto_decode_output_config(const std::uint8_t* data, std::size_t size,
                                           md_producer_config_t* config);
std::int32_t md_proto_decode_producer_gpu_bound(const std::uint8_t* data, std::size_t size,
                                                md_producer_gpu_info_t* gpu);
std::int32_t md_proto_decode_pointer_enter(const std::uint8_t* data, std::size_t size,
                                           md_pointer_enter_t* event);
std::int32_t md_proto_decode_pointer_leave(const std::uint8_t* data, std::size_t size,
                                           std::uint64_t* timestamp_us);
std::int32_t md_proto_decode_pointer_motion(const std::uint8_t* data, std::size_t size,
                                            md_pointer_motion_t* event);
std::int32_t md_proto_decode_pointer_button(const std::uint8_t* data, std::size_t size,
                                            md_pointer_button_t* event);
std::int32_t md_proto_decode_pointer_axis(const std::uint8_t* data, std::size_t size,
                                          md_pointer_axis_t* event);
std::int32_t md_proto_decode_window_state(const std::uint8_t* data, std::size_t size,
                                          std::uint32_t* flags);

#endif
