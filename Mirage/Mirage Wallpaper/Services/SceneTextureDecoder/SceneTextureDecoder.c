#include "SceneTextureDecoder.h"

#include <stddef.h>
#include <string.h>

// Vendored from MirageMobileAsst, including bcdec's original license text.
#define BCDECDEF static inline
#define BCDEC_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcomma"
#include "bcdec.h"
#pragma clang diagnostic pop

void mirage_decode_bc7(const uint8_t *source, uint8_t *rgba, int width, int height) {
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            // bcdec reads the compressed block as two 64-bit words.
            unsigned long long compressed[2];
            uint8_t pixels[4 * 4 * 4];
            memcpy(compressed, source, sizeof(compressed));
            bcdec_bc7(compressed, pixels, 4 * 4);
            source += sizeof(compressed);
            int rows = height - y < 4 ? height - y : 4;
            int columns = width - x < 4 ? width - x : 4;
            for (int row = 0; row < rows; ++row) {
                memcpy(rgba + ((size_t)(y + row) * width + x) * 4, pixels + row * 16, columns * 4);
            }
        }
    }
}
