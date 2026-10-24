#include "imaging.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dmm
{

static const uint8_t kPal[kPalCount][3] = {
#include "palette_data.inc"
};

void PaletteRgb(int index, uint8_t rgb[3])
{
    const uint8_t *p = kPal[index - kPalFirst];
    rgb[0] = p[0];
    rgb[1] = p[1];
    rgb[2] = p[2];
}

int NearestIndex(int r, int g, int b)
{
    int best = 0;
    int bestD = std::numeric_limits<int>::max();
    for (int i = 0; i < kPalCount; i++)
    {
        int dr = r - kPal[i][0], dg = g - kPal[i][1], db = b - kPal[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    return best + kPalFirst;
}

int PowerOfTwoSize(int n, int minSize, int maxSize)
{
    if (n < 1)
        n = 1;
    int p = 1;
    while (p < n)
        p <<= 1;
    int lo = p >> 1;
    // closer one in log space
    if (lo >= 1 && (double)n / lo < (double)p / n)
        p = lo;
    return std::max(minSize, std::min(maxSize, p));
}

RgbaImage Resample(const RgbaImage &src, int w, int h)
{
    RgbaImage dst;
    dst.w = w;
    dst.h = h;
    dst.px.assign((size_t)w * h * 4, 255);
    if (src.w == 0 || src.h == 0)
        return dst;
    // area-weighted box filter (exact for shrinking, becomes nearest-ish for enlarging)
    for (int y = 0; y < h; y++)
    {
        double y0 = (double)y * src.h / h, y1 = (double)(y + 1) * src.h / h;
        for (int x = 0; x < w; x++)
        {
            double x0 = (double)x * src.w / w, x1 = (double)(x + 1) * src.w / w;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int sy = (int)std::floor(y0); sy < (int)std::ceil(y1) && sy < src.h; sy++)
            {
                double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                for (int sx = (int)std::floor(x0); sx < (int)std::ceil(x1) && sx < src.w; sx++)
                {
                    double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                    double wt = wx * wy;
                    const uint8_t *p = &src.px[((size_t)sy * src.w + sx) * 4];
                    for (int c = 0; c < 4; c++)
                        acc[c] += p[c] * wt;
                    wsum += wt;
                }
            }
            uint8_t *o = &dst.px[((size_t)y * w + x) * 4];
            for (int c = 0; c < 4; c++)
                o[c] = (uint8_t)std::lround(wsum > 0 ? acc[c] / wsum : 0);
        }
    }
    return dst;
}

Image8 Quantize(const RgbaImage &src, bool dither)
{
    Image8 out;
    out.w = src.w;
    out.h = src.h;
    out.px.assign((size_t)src.w * src.h, kPalFirst);
    std::vector<float> err((size_t)(src.w + 2) * 2 * 3, 0.0f); // two rows of error, 3 channels, 1px padding
    auto E = [&](int row, int x, int c) -> float & { return err[((size_t)(row & 1) * (src.w + 2) + (x + 1)) * 3 + c]; };
    for (int y = 0; y < src.h; y++)
    {
        bool ltr = !dither || (y % 2 == 0);
        for (int i = 0; i < src.w; i++)
        {
            int x = ltr ? i : src.w - 1 - i;
            const uint8_t *p = &src.px[((size_t)y * src.w + x) * 4];
            if (p[3] < 128)
            {   // transparent: palette index 0 is the one transparent colour of a texture
                out.px[(size_t)y * src.w + x] = 0;
                continue;
            }
            float v[3];
            for (int c = 0; c < 3; c++)
            {
                v[c] = p[c] + (dither ? E(y, x, c) : 0.0f);
                v[c] = std::max(0.0f, std::min(255.0f, v[c]));
            }
            int idx = NearestIndex((int)std::lround(v[0]), (int)std::lround(v[1]), (int)std::lround(v[2]));
            out.px[(size_t)y * src.w + x] = (uint8_t)idx;
            if (dither)
            {
                const uint8_t *q = kPal[idx - kPalFirst];
                for (int c = 0; c < 3; c++)
                {
                    float e = v[c] - q[c];
                    int dx = ltr ? 1 : -1;
                    E(y, x + dx, c) += e * 7.0f / 16.0f;
                    E(y + 1, x - dx, c) += e * 3.0f / 16.0f;
                    E(y + 1, x, c) += e * 5.0f / 16.0f;
                    E(y + 1, x + dx, c) += e * 1.0f / 16.0f;
                }
            }
        }
        if (dither)
            for (int x = -1; x <= src.w; x++)
                for (int c = 0; c < 3; c++)
                    E(y, x, c) = 0.0f; // this row's errors have been consumed; the buffer row becomes y+2's
    }
    return out;
}

Image8 MakeThumbFromImage(const RgbaImage &src)
{
    const int S = 72;
    RgbaImage canvas;
    canvas.w = canvas.h = S;
    canvas.px.assign((size_t)S * S * 4, 255);
    for (size_t i = 0; i < (size_t)S * S; i++)
    {
        canvas.px[i * 4 + 0] = canvas.px[i * 4 + 1] = canvas.px[i * 4 + 2] = 96;
    }
    if (src.w > 0 && src.h > 0)
    {
        double sc = std::min((double)S / src.w, (double)S / src.h);
        int w = std::max(1, (int)std::lround(src.w * sc)), h = std::max(1, (int)std::lround(src.h * sc));
        RgbaImage r = Resample(src, w, h);
        int ox = (S - w) / 2, oy = (S - h) / 2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const uint8_t *p = &r.px[((size_t)y * w + x) * 4];
                uint8_t *o = &canvas.px[((size_t)(y + oy) * S + (x + ox)) * 4];
                float a = p[3] / 255.0f;
                for (int c = 0; c < 3; c++)
                    o[c] = (uint8_t)std::lround(p[c] * a + o[c] * (1 - a));
            }
    }
    return Quantize(canvas, false);
}

Image8 RenderThumb(const std::vector<RenderPart> &parts, int S)
{
    Image8 out;
    out.w = out.h = S;
    int bg = NearestIndex(96, 96, 96);
    out.px.assign((size_t)S * S, (uint8_t)bg);

    // bounds in screen space (sx = z, sy = y)
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (const RenderPart &p : parts)
        for (const Vertex &v : p.mesh->verts)
        {
            minx = std::min(minx, v.z);
            maxx = std::max(maxx, v.z);
            miny = std::min(miny, v.y);
            maxy = std::max(maxy, v.y);
        }
    if (minx > maxx)
        return out;
    float margin = 4.0f;
    float sc = std::min((S - 2 * margin) / std::max(1e-3f, maxx - minx), (S - 2 * margin) / std::max(1e-3f, maxy - miny));
    float ox = (S - (maxx - minx) * sc) / 2 - minx * sc, oy = (S - (maxy - miny) * sc) / 2 - miny * sc;

    std::vector<float> zbuf((size_t)S * S, 1e30f);
    int grey = NearestIndex(170, 170, 170);
    for (const RenderPart &p : parts)
    {
        for (const auto &f : p.mesh->faces)
        {
            const Vertex *V[3] = {&p.mesh->verts[f[0]], &p.mesh->verts[f[1]], &p.mesh->verts[f[2]]};
            float X[3], Y[3];
            for (int i = 0; i < 3; i++)
            {
                X[i] = V[i]->z * sc + ox;
                Y[i] = (S - 1) - (V[i]->y * sc + oy); // image rows go down
            }
            float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
            // image y points down, so a counter-clockwise (front) triangle has negative area here
            if (area >= 0)
                continue;
            int x0 = std::max(0, (int)std::floor(std::min({X[0], X[1], X[2]})));
            int x1 = std::min(S - 1, (int)std::ceil(std::max({X[0], X[1], X[2]})));
            int y0 = std::max(0, (int)std::floor(std::min({Y[0], Y[1], Y[2]})));
            int y1 = std::min(S - 1, (int)std::ceil(std::max({Y[0], Y[1], Y[2]})));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                {
                    float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
                    float w1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;
                    float depth = w0 * V[0]->x + w1 * V[1]->x + w2 * V[2]->x; // camera at -X: smaller x is nearer
                    size_t o = (size_t)y * S + x;
                    if (depth >= zbuf[o])
                        continue;
                    uint8_t val = (uint8_t)grey;
                    if (p.tex && p.tex->w > 0)
                    {
                        float u = w0 * V[0]->u + w1 * V[1]->u + w2 * V[2]->u;
                        float v = w0 * V[0]->v + w1 * V[1]->v + w2 * V[2]->v;
                        u -= std::floor(u);
                        v -= std::floor(v);
                        int tx = std::min(p.tex->w - 1, (int)(u * p.tex->w));
                        int ty = std::min(p.tex->h - 1, (int)((1.0f - v) * p.tex->h)); // Image8 rows are top-down
                        val = p.tex->px[(size_t)ty * p.tex->w + tx];
                        if (val == 0)
                            continue; // transparent texel
                    }
                    zbuf[o] = depth;
                    out.px[o] = val;
                }
        }
    }
    return out;
}

} // namespace dmm
