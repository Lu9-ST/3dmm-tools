// Palette quantisation, resampling and the little software renderer used for the picker thumbnail.
#ifndef DMM_IMAGING_H
#define DMM_IMAGING_H

#include "actor.h"

namespace dmm
{

// 8-bit RGBA image, rows top-down
struct RgbaImage
{
    int w = 0, h = 0;
    std::vector<uint8_t> px; // w*h*4
};

const int kPalFirst = 10; // first shared actor palette index
const int kPalCount = 146;

void PaletteRgb(int index, uint8_t rgb[3]); // index in 10..155
int NearestIndex(int r, int g, int b);      // returns a palette index (10..155)

// Area-average / bilinear resample to w x h.
RgbaImage Resample(const RgbaImage &src, int w, int h);

// Nearest power of two (rounding to the closer one in log space), clamped to [minSize, maxSize].
int PowerOfTwoSize(int n, int minSize, int maxSize);

// Convert to the shared palette.  dither: Floyd-Steinberg (serpentine).
Image8 Quantize(const RgbaImage &src, bool dither);

// Thumbnail: letterbox an arbitrary image into 72x72.
Image8 MakeThumbFromImage(const RgbaImage &src);

struct RenderPart
{
    const Mesh *mesh;
    const Image8 *tex; // null => flat grey
};

// Orthographic render, camera on the -X side looking toward +X (the side stock actors face), Y up.
Image8 RenderThumb(const std::vector<RenderPart> &parts, int size = 72);

} // namespace dmm

#endif
