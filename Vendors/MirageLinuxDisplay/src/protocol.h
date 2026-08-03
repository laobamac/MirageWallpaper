#ifndef MIRAGE_DISPLAY_PROTOCOL_H
#define MIRAGE_DISPLAY_PROTOCOL_H

#include "mirage_display.h"
#include "mirage_display_producer.h"

#include <stddef.h>
#include <stdint.h>

enum md_opcode {
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
};

typedef struct md_writer {
    uint8_t* data;
    size_t capacity;
    size_t size;
} md_writer_t;

typedef struct md_reader {
    const uint8_t* data;
    size_t size;
    size_t offset;
} md_reader_t;

typedef struct md_proto_welcome {
    uint16_t selected_minor;
    uint64_t features;
    char* server_name;
    char* server_version;
} md_proto_welcome_t;

typedef struct md_proto_error {
    uint32_t code;
    bool fatal;
    char* message;
} md_proto_error_t;

void md_writer_init(md_writer_t* writer, uint8_t* data, size_t capacity);
int md_write_u16(md_writer_t* writer, uint16_t value);
int md_write_u32(md_writer_t* writer, uint32_t value);
int md_write_u64(md_writer_t* writer, uint64_t value);
int md_write_f32(md_writer_t* writer, float value);
int md_write_bytes(md_writer_t* writer, const void* data, size_t size);
int md_write_string(md_writer_t* writer, const char* value);

void md_reader_init(md_reader_t* reader, const uint8_t* data, size_t size);
int md_read_u16(md_reader_t* reader, uint16_t* value);
int md_read_u32(md_reader_t* reader, uint32_t* value);
int md_read_u64(md_reader_t* reader, uint64_t* value);
int md_read_f32(md_reader_t* reader, float* value);
int md_read_bytes(md_reader_t* reader, void* data, size_t size);
int md_read_string(md_reader_t* reader, char** value);
int md_reader_finish(const md_reader_t* reader);

int md_proto_encode_hello(md_writer_t* writer, uint32_t role, const char* name,
                          const char* version, uint64_t features);
int md_proto_encode_register_output(md_writer_t* writer, const md_output_info_t* output);
int md_proto_encode_update_output(md_writer_t* writer, const md_output_info_t* output);
int md_proto_encode_consumer_caps(md_writer_t* writer, const md_consumer_caps_t* caps);
int md_proto_encode_pointer_enter(md_writer_t* writer, float x, float y, uint64_t timestamp_us);
int md_proto_encode_pointer_leave(md_writer_t* writer, uint64_t timestamp_us);
int md_proto_encode_pointer_motion(md_writer_t* writer, float x, float y, uint64_t timestamp_us,
                                   uint32_t modifiers);
int md_proto_encode_pointer_button(md_writer_t* writer, float x, float y, uint32_t button,
                                   md_button_state_t state, uint64_t timestamp_us,
                                   uint32_t modifiers);
int md_proto_encode_pointer_axis(md_writer_t* writer, float x, float y, float delta_x,
                                 float delta_y, md_axis_source_t source, uint64_t timestamp_us,
                                 uint32_t modifiers);
int md_proto_encode_u32(md_writer_t* writer, uint32_t value);
int md_proto_encode_u64(md_writer_t* writer, uint64_t value);
int md_proto_encode_register_producer(md_writer_t* writer, const md_producer_info_t* info);
int md_proto_encode_offer_buffers(md_writer_t* writer, const md_buffer_pool_t* pool);
int md_proto_encode_producer_frame(md_writer_t* writer, uint64_t generation,
                                   uint32_t buffer_index, uint64_t sequence);
int md_proto_encode_config(md_writer_t* writer, const md_display_config_t* config);

int md_proto_decode_welcome(const uint8_t* data, size_t size, md_proto_welcome_t* welcome);
void md_proto_welcome_clear(md_proto_welcome_t* welcome);
int md_proto_decode_error(const uint8_t* data, size_t size, md_proto_error_t* error);
void md_proto_error_clear(md_proto_error_t* error);
int md_proto_decode_output_accepted(const uint8_t* data, size_t size, uint64_t* output_id);
int md_proto_decode_bind_buffers(const uint8_t* data, size_t size, md_buffer_pool_t* pool);
int md_proto_decode_config(const uint8_t* data, size_t size, md_display_config_t* config);
int md_proto_decode_frame(const uint8_t* data, size_t size, md_frame_t* frame);
int md_proto_decode_unbind(const uint8_t* data, size_t size, uint64_t* generation);
int md_proto_decode_producer_accepted(const uint8_t* data, size_t size, uint64_t* producer_id,
                                      uint64_t* output_id);
int md_proto_decode_output_config(const uint8_t* data, size_t size,
                                  md_producer_config_t* config);
int md_proto_decode_pointer_enter(const uint8_t* data, size_t size, md_pointer_enter_t* event);
int md_proto_decode_pointer_leave(const uint8_t* data, size_t size, uint64_t* timestamp_us);
int md_proto_decode_pointer_motion(const uint8_t* data, size_t size,
                                   md_pointer_motion_t* event);
int md_proto_decode_pointer_button(const uint8_t* data, size_t size,
                                   md_pointer_button_t* event);
int md_proto_decode_pointer_axis(const uint8_t* data, size_t size, md_pointer_axis_t* event);

#endif
