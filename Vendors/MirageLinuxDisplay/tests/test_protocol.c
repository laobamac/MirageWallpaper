#include "protocol.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_hello_golden_vector(void) {
    uint8_t bytes[128];
    md_writer_t writer;
    md_writer_init(&writer, bytes, sizeof(bytes));
    assert(md_proto_encode_hello(&writer, 1, "client", "1.0", MD_FEATURE_EXPLICIT_SYNC) == 0);

    const uint8_t expected[] = {
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x06, 0x00, 0x00, 0x00, 'c', 'l', 'i', 'e', 'n', 't',
        0x03, 0x00, 0x00, 0x00, '1', '.', '0',
    };
    assert(writer.size == sizeof(expected));
    assert(memcmp(bytes, expected, sizeof(expected)) == 0);
}

static void test_welcome_decode(void) {
    uint8_t bytes[128];
    md_writer_t writer;
    md_writer_init(&writer, bytes, sizeof(bytes));
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u64(&writer, MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS) == 0);
    assert(md_write_string(&writer, "mock-broker") == 0);
    assert(md_write_string(&writer, "0.1") == 0);

    md_proto_welcome_t welcome;
    assert(md_proto_decode_welcome(bytes, writer.size, &welcome) == 0);
    assert(welcome.selected_minor == 0);
    assert((welcome.features & MD_FEATURE_EXPLICIT_SYNC) != 0);
    assert(strcmp(welcome.server_name, "mock-broker") == 0);
    assert(strcmp(welcome.server_version, "0.1") == 0);
    md_proto_welcome_clear(&welcome);

    bytes[writer.size++] = 0xff;
    assert(md_proto_decode_welcome(bytes, writer.size, &welcome) != 0);
}

static void test_bind_decode(void) {
    uint8_t bytes[512];
    md_writer_t writer;
    md_writer_init(&writer, bytes, sizeof(bytes));
    assert(md_write_u64(&writer, 9) == 0);
    assert(md_write_u32(&writer, 3) == 0);
    assert(md_write_u32(&writer, 1920) == 0);
    assert(md_write_u32(&writer, 1080) == 0);
    assert(md_write_u32(&writer, UINT32_C(0x34325258)) == 0);
    assert(md_write_u32(&writer, 1) == 0);
    assert(md_write_u64(&writer, 0) == 0);
    assert(md_write_u32(&writer, 3) == 0);
    for (uint32_t i = 0; i < 3; ++i) {
        assert(md_write_u32(&writer, 7680) == 0);
        assert(md_write_u32(&writer, i * 4096) == 0);
        assert(md_write_u64(&writer, UINT64_C(8294400)) == 0);
    }

    md_buffer_pool_t pool;
    assert(md_proto_decode_bind_buffers(bytes, writer.size, &pool) == 0);
    assert(pool.generation == 9);
    assert(pool.buffer_count == 3);
    assert(pool.plane_count == 1);
    assert(pool.planes[2][0].offset == 8192);
    assert(pool.planes[0][0].fd == -1);

    bytes[36] = 2;
    assert(md_proto_decode_bind_buffers(bytes, writer.size, &pool) != 0);
}

static void test_multiplane_bind_round_trip(void) {
    md_buffer_pool_t source;
    memset(&source, 0, sizeof(source));
    source.generation = 11;
    source.buffer_count = 2;
    source.width = 1280;
    source.height = 720;
    source.fourcc = UINT32_C(0x3231564e); /* NV12 */
    source.plane_count = 2;
    source.modifier = UINT64_C(0x0102030405060708);
    for (uint32_t b = 0; b < source.buffer_count; ++b) {
        for (uint32_t p = 0; p < source.plane_count; ++p) {
            source.planes[b][p].fd = -1;
            source.planes[b][p].stride = p == 0 ? 1280 : 1280;
            source.planes[b][p].offset = p == 0 ? 0 : 921600;
            source.planes[b][p].size = p == 0 ? 921600 : 460800;
        }
    }
    uint8_t bytes[512];
    md_writer_t writer;
    md_writer_init(&writer, bytes, sizeof(bytes));
    assert(md_proto_encode_offer_buffers(&writer, &source) == 0);

    md_buffer_pool_t decoded;
    assert(md_proto_decode_bind_buffers(bytes, writer.size, &decoded) == 0);
    assert(decoded.generation == source.generation);
    assert(decoded.buffer_count == 2);
    assert(decoded.plane_count == 2);
    assert(decoded.modifier == source.modifier);
    assert(decoded.planes[0][1].offset == 921600);
    assert(decoded.planes[1][0].stride == 1280);
}

static void test_invalid_utf8(void) {
    uint8_t bytes[32];
    md_writer_t writer;
    md_writer_init(&writer, bytes, sizeof(bytes));
    const char invalid[] = {(char)0xc0, (char)0x80, '\0'};
    assert(md_write_string(&writer, invalid) != 0);
}

int main(void) {
    test_hello_golden_vector();
    test_welcome_decode();
    test_bind_decode();
    test_multiplane_bind_round_trip();
    test_invalid_utf8();
    return 0;
}
