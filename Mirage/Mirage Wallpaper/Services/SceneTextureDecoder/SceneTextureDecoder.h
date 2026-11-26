#ifndef MIRAGE_SCENE_TEXTURE_DECODER_H
#define MIRAGE_SCENE_TEXTURE_DECODER_H

#include <stdint.h>

void mirage_decode_bc7(const uint8_t *source, uint8_t *rgba, int width, int height);

#endif
