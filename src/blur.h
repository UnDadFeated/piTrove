#ifndef PITROVE_BLUR_H
#define PITROVE_BLUR_H

#include "image_loader.h"

// Separable box blur: 3-pass (horiz → vert → horiz) for near-Gaussian quality
RawImage box_blur(const RawImage& src, int radius);

// Compute matte color from center 50% of image to avoid border artifacts
void compute_matte_color(const RawImage& src, uint8_t& r, uint8_t& g, uint8_t& b);

#endif // PITROVE_BLUR_H
