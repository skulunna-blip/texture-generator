#include "PatchSynthesis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <thread>
#include <vector>

namespace
{
inline int wrapI(int v, int n)
{
    if (v >= 0 && v < n) {
        return v;
    }
    v %= n;
    return v < 0 ? v + n : v;
}

struct Field
{
    int w = 0;
    int h = 0;
    std::vector<float> p; // three floats per pixel

    void reset(int width, int height)
    {
        w = width;
        h = height;
        p.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0.0f);
    }
    float* at(int x, int y) { return &p[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3]; }
    const float* at(int x, int y) const { return &p[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3]; }
};

struct Level
{
    Field field;
    std::vector<unsigned char> hole;    // 1 = synthesized
    std::vector<unsigned char> blocked; // 1 = a source patch must not be centred here
};

Field fieldFromImage(const QImage& image)
{
    Field f;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    f.reset(rgba.width(), rgba.height());
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* line = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            float* d = f.at(x, y);
            d[0] = line[x * 4 + 0] / 255.0f;
            d[1] = line[x * 4 + 1] / 255.0f;
            d[2] = line[x * 4 + 2] / 255.0f;
        }
    }
    return f;
}

// Separable dilation of the hole by `radius` (Chebyshev, which is exactly the
// square patch footprint). A source patch may not overlap the hole even by one
// pixel: the hole's own contents are the estimate we are trying to replace, and
// copying from it would just move the guess around.
void buildBlocked(Level& level, int radius, bool wrap)
{
    const int w = level.field.w;
    const int h = level.field.h;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<unsigned char> horizontal(n, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char any = 0;
            for (int d = -radius; d <= radius && any == 0; ++d) {
                const int xx = wrap ? wrapI(x + d, w) : std::clamp(x + d, 0, w - 1);
                any = level.hole[static_cast<size_t>(y) * w + xx];
            }
            horizontal[static_cast<size_t>(y) * w + x] = any;
        }
    }
    level.blocked.assign(n, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char any = 0;
            for (int d = -radius; d <= radius && any == 0; ++d) {
                const int yy = wrap ? wrapI(y + d, h) : std::clamp(y + d, 0, h - 1);
                any = horizontal[static_cast<size_t>(yy) * w + x];
            }
            level.blocked[static_cast<size_t>(y) * w + x] = any;
        }
    }
}

// Nearest-known-pixel fill: the starting guess at the coarsest level. The
// search refines it, but starting from noise makes the first aggregation round
// paste unrelated patches everywhere and the coarse levels never recover.
//
// The walk has to respect the wrap when searching does, or the guess fights the
// match: on a horizontal ramp the pixel at x=0 is one step from the last column
// (bright) and twenty-five steps from the band's inner edge (dark), so a
// non-wrapping walk seeds it from the dark side. The patch around it then reads
// "bright, bright, bright, dark, dark" - a combination that exists nowhere in
// the picture, which no source matches, and the search settles on a compromise
// level that leaves half the seam's step behind (measured 48 levels).
void seedFromNearest(Field& field, const std::vector<unsigned char>& hole, bool wrap)
{
    const int w = field.w;
    const int h = field.h;
    std::vector<int> queue;
    std::vector<unsigned char> seen(hole.size(), 0);
    queue.reserve(hole.size());
    for (size_t i = 0; i < hole.size(); ++i) {
        if (hole[i] == 0) {
            queue.push_back(static_cast<int>(i));
            seen[i] = 1;
        }
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        const int index = queue[head];
        const int x = index % w;
        const int y = index / w;
        const float* src = field.at(x, y);
        const float r = src[0];
        const float g = src[1];
        const float b = src[2];
        const int nx[4] = { x - 1, x + 1, x, x };
        const int ny[4] = { y, y, y - 1, y + 1 };
        for (int k = 0; k < 4; ++k) {
            int xx = nx[k];
            int yy = ny[k];
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
                if (!wrap) {
                    continue;
                }
                xx = wrapI(xx, w);
                yy = wrapI(yy, h);
            }
            const size_t j = static_cast<size_t>(yy) * w + xx;
            if (seen[j] != 0) {
                continue;
            }
            seen[j] = 1;
            float* dst = field.at(xx, yy);
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            queue.push_back(static_cast<int>(j));
        }
    }
}

// Smooth the seed into a harmonic fill.
//
// The nearest-pixel walk leaves a visible ledge where the fills arriving from
// two sides of the hole meet. Every target patch straddling that ledge is then
// a patch that does not exist in the picture, so the search has nothing good to
// copy and averages the two sides instead. Solving Laplace's equation on the
// hole (Jacobi; the holes are small and this only runs once per pyramid) gives
// a seed that is continuous everywhere, which is what the first match needs.
// Whatever texture belongs there comes back from the patches - this only
// decides where the levels meet.
void relaxSeed(Field& field, const std::vector<unsigned char>& hole, bool wrap, int iterations)
{
    const int w = field.w;
    const int h = field.h;
    if (w < 3 || h < 3) {
        return;
    }
    Field next = field;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t index = static_cast<size_t>(y) * w + x;
                if (hole[index] == 0) {
                    continue;
                }
                const int nx[4] = { wrapI(x - 1, w), wrapI(x + 1, w), x, x };
                const int ny[4] = { y, y, wrapI(y - 1, h), wrapI(y + 1, h) };
                float sum[3] = { 0.0f, 0.0f, 0.0f };
                for (int k = 0; k < 4; ++k) {
                    const float* n = field.at(nx[k], ny[k]);
                    sum[0] += n[0];
                    sum[1] += n[1];
                    sum[2] += n[2];
                }
                float* d = next.at(x, y);
                d[0] = sum[0] * 0.25f;
                d[1] = sum[1] * 0.25f;
                d[2] = sum[2] * 0.25f;
            }
        }
        std::swap(field.p, next.p);
    }
}

Field downsample(const Field& src, const std::vector<unsigned char>& hole,
                 std::vector<unsigned char>& outHole)
{
    Field out;
    const int w = std::max(1, src.w / 2);
    const int h = std::max(1, src.h / 2);
    out.reset(w, h);
    outHole.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            int count = 0;
            unsigned char anyHole = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(src.w - 1, x * 2 + dx);
                    const int sy = std::min(src.h - 1, y * 2 + dy);
                    const float* p = src.at(sx, sy);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    anyHole |= hole[static_cast<size_t>(sy) * src.w + sx];
                    ++count;
                }
            }
            float* d = out.at(x, y);
            d[0] = r / count;
            d[1] = g / count;
            d[2] = b / count;
            outHole[static_cast<size_t>(y) * w + x] = anyHole;
        }
    }
    return out;
}

// Bilinear upsample of the coarser estimate, used as this level's starting
// guess (the E step of every patch-based EM scheme).
void upsampleInto(const Field& src, Field& dst)
{
    for (int y = 0; y < dst.h; ++y) {
        const float fy = (y + 0.5f) * 0.5f - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src.h - 1);
        const int y1 = std::min(src.h - 1, y0 + 1);
        const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int x = 0; x < dst.w; ++x) {
            const float fx = (x + 0.5f) * 0.5f - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src.w - 1);
            const int x1 = std::min(src.w - 1, x0 + 1);
            const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
            const float* p00 = src.at(x0, y0);
            const float* p10 = src.at(x1, y0);
            const float* p01 = src.at(x0, y1);
            const float* p11 = src.at(x1, y1);
            float* d = dst.at(x, y);
            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] * (1.0f - tx) + p10[c] * tx;
                const float bottom = p01[c] * (1.0f - tx) + p11[c] * tx;
                d[c] = top * (1.0f - ty) + bottom * ty;
            }
        }
    }
}

// One search + aggregate round (the M step).
void refine(Field& est, const Level& level, const PatchSynthOptions& options)
{
    const int w = est.w;
    const int h = est.h;
    const int radius = options.patchRadius;
    const bool wrap = options.wrap;
    const auto& hole = level.hole;
    const auto& blocked = level.blocked;

    std::vector<int> sourceX(hole.size(), -1);
    std::vector<int> sourceY(hole.size(), -1);

    std::vector<int> valid;
    valid.reserve(hole.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (blocked[static_cast<size_t>(y) * w + x] == 0) {
                valid.push_back(y * w + x);
            }
        }
    }
    if (valid.empty()) {
        return;
    }

    std::mt19937 rng(0x9e3779b9u);
    std::uniform_int_distribution<int> pick(0, static_cast<int>(valid.size()) - 1);
    for (size_t i = 0; i < hole.size(); ++i) {
        if (hole[i] != 0) {
            const int s = valid[pick(rng)];
            sourceX[i] = s % w;
            sourceY[i] = s / w;
        }
    }

    auto costAt = [&](int tx, int ty, int sx0, int sy0, double limit) {
        double sum = 0.0;
        for (int dy = -radius; dy <= radius; ++dy) {
            const int tyy = wrap ? wrapI(ty + dy, h) : std::clamp(ty + dy, 0, h - 1);
            const int syy = wrap ? wrapI(sy0 + dy, h) : std::clamp(sy0 + dy, 0, h - 1);
            for (int dx = -radius; dx <= radius; ++dx) {
                const int txx = wrap ? wrapI(tx + dx, w) : std::clamp(tx + dx, 0, w - 1);
                const int sxx = wrap ? wrapI(sx0 + dx, w) : std::clamp(sx0 + dx, 0, w - 1);
                const float* a = est.at(txx, tyy);
                const float* b = est.at(sxx, syy);
                const double d0 = static_cast<double>(a[0]) - b[0];
                const double d1 = static_cast<double>(a[1]) - b[1];
                const double d2 = static_cast<double>(a[2]) - b[2];
                sum += d0 * d0 + d1 * d1 + d2 * d2;
                if (sum > limit) {
                    return sum;
                }
            }
        }
        return sum;
    };

    auto tryCandidate = [&](int index, int cx, int cy, double& best, int& bx, int& by) {
        cx = wrap ? wrapI(cx, w) : std::clamp(cx, 0, w - 1);
        cy = wrap ? wrapI(cy, h) : std::clamp(cy, 0, h - 1);
        if (blocked[static_cast<size_t>(cy) * w + cx] != 0) {
            return;
        }
        const int x = static_cast<int>(index % w);
        const int y = static_cast<int>(index / w);
        const double cost = costAt(x, y, cx, cy, best);
        if (cost < best) {
            best = cost;
            bx = cx;
            by = cy;
        }
    };

    // Propagation is red-black: within a pass only one checkerboard parity is
    // touched, so every neighbour a pixel reads still holds its previous match
    // and the pass is race-free. That is what lets the rows be spread over all
    // the cores - a classic sequential scan can't be, and it was the whole
    // cost: 18 s for a 1024 tile.
    const int threadCount = std::max(1, std::min<int>(
        static_cast<int>(std::thread::hardware_concurrency()), std::max(1, h / 16)));
    auto forEachRow = [&](auto&& body) {
        if (threadCount <= 1) {
            for (int y = 0; y < h; ++y) {
                body(y);
            }
            return;
        }
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(threadCount));
        for (int t = 0; t < threadCount; ++t) {
            const int y0 = h * t / threadCount;
            const int y1 = h * (t + 1) / threadCount;
            pool.emplace_back([&, y0, y1]() {
                for (int y = y0; y < y1; ++y) {
                    body(y);
                }
            });
        }
        for (std::thread& worker : pool) {
            worker.join();
        }
    };

    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        for (int parity = 0; parity < 2; ++parity) {
            forEachRow([&](int y) {
                std::mt19937 rowRng(static_cast<unsigned>(y) * 2654435761u + static_cast<unsigned>(parity) + 17u);
                for (int x = (y + parity) & 1; x < w; x += 2) {
                    const size_t index = static_cast<size_t>(y) * w + x;
                    if (hole[index] == 0) {
                        continue;
                    }
                    int bx = sourceX[index];
                    int by = sourceY[index];
                    double best = costAt(x, y, bx, by, std::numeric_limits<double>::infinity());

                    // Propagate from the four neighbours: a neighbour that
                    // found a good patch is usually one pixel away from this
                    // pixel's good patch too.
                    const int nx[4] = { x - 1, x + 1, x, x };
                    const int ny[4] = { y, y, y - 1, y + 1 };
                    for (int k = 0; k < 4; ++k) {
                        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) {
                            continue;
                        }
                        const size_t nIndex = static_cast<size_t>(ny[k]) * w + nx[k];
                        if (sourceX[nIndex] < 0) {
                            continue;
                        }
                        tryCandidate(index, sourceX[nIndex] + (x - nx[k]),
                                     sourceY[nIndex] + (y - ny[k]), best, bx, by);
                    }

                    // Random search, coarse to fine.
                    int step = std::max(w, h);
                    while (step > 1) {
                        std::uniform_int_distribution<int> off(-step, step);
                        tryCandidate(index, bx + off(rowRng), by + off(rowRng), best, bx, by);
                        step /= 2;
                    }

                    sourceX[index] = bx;
                    sourceY[index] = by;
                }
            });
        }

        // Aggregate. Voting averages every patch that covers a pixel, which is
        // what removes the visible patch borders; copying keeps the sharper
        // single best.
        if (options.vote) {
            std::vector<float> sum(static_cast<size_t>(w) * h * 3, 0.0f);
            std::vector<float> count(static_cast<size_t>(w) * h, 0.0f);
            for (size_t i = 0; i < hole.size(); ++i) {
                if (sourceX[i] < 0) {
                    continue;
                }
                const int sx = sourceX[i];
                const int sy = sourceY[i];
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int tx = static_cast<int>(i % w) + dx;
                        const int ty = static_cast<int>(i / w) + dy;
                        if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
                            continue;
                        }
                        const int ux = wrap ? wrapI(sx + dx, w) : std::clamp(sx + dx, 0, w - 1);
                        const int uy = wrap ? wrapI(sy + dy, h) : std::clamp(sy + dy, 0, h - 1);
                        const float* src = est.at(ux, uy);
                        float* acc = &sum[(static_cast<size_t>(ty) * w + tx) * 3];
                        acc[0] += src[0];
                        acc[1] += src[1];
                        acc[2] += src[2];
                        count[static_cast<size_t>(ty) * w + tx] += 1.0f;
                    }
                }
            }
            for (size_t i = 0; i < hole.size(); ++i) {
                if (hole[i] == 0) {
                    continue;
                }
                const float c = count[i];
                if (c <= 0.0f) {
                    continue;
                }
                float* d = &est.p[i * 3];
                const float* acc = &sum[i * 3];
                d[0] = acc[0] / c;
                d[1] = acc[1] / c;
                d[2] = acc[2] / c;
            }
        } else {
            for (size_t i = 0; i < hole.size(); ++i) {
                if (hole[i] == 0 || sourceX[i] < 0) {
                    continue;
                }
                const float* src = est.at(sourceX[i], sourceY[i]);
                float* d = &est.p[i * 3];
                d[0] = src[0];
                d[1] = src[1];
                d[2] = src[2];
            }
        }
    }
}

} // namespace

QImage patchSynthesis(const QImage& image, const QImage& mask, const PatchSynthOptions& options)
{
    if (image.isNull() || mask.isNull()) {
        return image;
    }
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888);
    const QImage maskImage = mask.scaled(source.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation)
                                 .convertToFormat(QImage::Format_Grayscale8);
    const int w = source.width();
    const int h = source.height();
    if (w < 4 || h < 4) {
        return image;
    }

    Level finest;
    finest.field = fieldFromImage(source);
    finest.hole.assign(static_cast<size_t>(w) * h, 0);
    bool anyHole = false;
    for (int y = 0; y < h; ++y) {
        const uchar* line = maskImage.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (line[x] > 127) {
                finest.hole[static_cast<size_t>(y) * w + x] = 1;
                anyHole = true;
            }
        }
    }
    if (!anyHole) {
        return image;
    }

    std::vector<Level> pyramid;
    pyramid.push_back(std::move(finest));

    const int minimumSide = std::max(16, options.patchRadius * 8);
    int levelLimit = options.levels > 0 ? options.levels : 64;
    while (options.multiscale && levelLimit > 1 && pyramid.size() < static_cast<size_t>(levelLimit)) {
        const Level& previous = pyramid.back();
        if (std::min(previous.field.w, previous.field.h) < minimumSide * 2) {
            break;
        }
        Level next;
        next.field = downsample(previous.field, previous.hole, next.hole);
        if (std::min(next.field.w, next.field.h) < minimumSide) {
            break;
        }
        pyramid.push_back(std::move(next));
        --levelLimit;
    }
    for (Level& level : pyramid) {
        buildBlocked(level, options.patchRadius, options.wrap);
    }

    // Coarsest first: the arrangement of whole structures is decided there,
    // and every finer level only refines what is already roughly right.
    Field estimate = pyramid.back().field;
    seedFromNearest(estimate, pyramid.back().hole, options.wrap);
    // Only the coarsest level: the hole there is a few pixels wide, so the
    // relaxation converges in a few dozen sweeps, and every level above it
    // starts from the bilinear upsample of an already-continuous guess.
    relaxSeed(estimate, pyramid.back().hole, options.wrap, 200);
    for (size_t index = pyramid.size(); index-- > 0;) {
        const Level& level = pyramid[index];
        if (index + 1 < pyramid.size()) {
            Field upsampled;
            upsampled.reset(level.field.w, level.field.h);
            upsampleInto(estimate, upsampled);
            // This level's known pixels are the real ones; only the hole takes
            // the coarser level's guess.
            estimate = level.field;
            for (size_t i = 0; i < level.hole.size(); ++i) {
                if (level.hole[i] != 0) {
                    estimate.p[i * 3 + 0] = upsampled.p[i * 3 + 0];
                    estimate.p[i * 3 + 1] = upsampled.p[i * 3 + 1];
                    estimate.p[i * 3 + 2] = upsampled.p[i * 3 + 2];
                }
            }
        }
        // refine() runs options.iterations rounds of search+aggregate itself.
        // This used to be wrapped in a second loop over the same count, which
        // re-seeded the match field every time: nine rounds of work per level,
        // and each one threw away what the previous one had found.
        //
        // The coarse levels always vote, even in copy mode. A single-pixel
        // copy leaves whatever the match got wrong in place, and the level
        // above it inherits the mistake and matches against it; averaging the
        // overlapping patches on the coarse levels keeps the estimate coherent
        // so the finest level has something consistent to copy from. Copying
        // at the top is still what keeps the detail sharp.
        PatchSynthOptions levelOptions = options;
        levelOptions.vote = options.vote || index > 0;
        refine(estimate, level, levelOptions);
    }

    QImage output = image.convertToFormat(QImage::Format_RGBA8888);
    const std::vector<unsigned char>& hole = pyramid.front().hole;
    for (int y = 0; y < h; ++y) {
        uchar* line = output.scanLine(y);
        for (int x = 0; x < w; ++x) {
            if (hole[static_cast<size_t>(y) * w + x] == 0) {
                continue;
            }
            const float* p = estimate.at(x, y);
            line[x * 4 + 0] = static_cast<uchar>(std::clamp(p[0] * 255.0f + 0.5f, 0.0f, 255.0f));
            line[x * 4 + 1] = static_cast<uchar>(std::clamp(p[1] * 255.0f + 0.5f, 0.0f, 255.0f));
            line[x * 4 + 2] = static_cast<uchar>(std::clamp(p[2] * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    return source.size() == image.size() ? output.convertToFormat(image.format()) : image;
}
