#include "LightingRemoval.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
constexpr double kEps = 1e-6;

double saturate(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

// Lighting is multiplicative, so every stage below works in linear light.
// Doing this in sRGB space (as a naive brightness tweak would) darkens the
// midtones and skews the hue of anything that gets scaled.
double srgbToLinearExact(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linearToSrgbExact(double c)
{
    const double v = saturate(c);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// The transfer functions above are pure per-channel maps, but calling them per
// pixel meant 6.3M std::pow() calls on a 1024^2 image - 650 ms of an 817 ms
// pass, 80% of the whole de-lighting cost. Both directions are table-driven
// instead, and both are exact rather than approximate.
//
// Decoding is exact for free: its input really is a byte, so 256 entries cover
// it completely.
//
// Encoding is the awkward one, because its input is a float. Approximating
// linearToSrgb() and rounding afterwards is *not* good enough here - measured
// against the pow version it moved 4 of 4.19M output bytes by up to 2, and the
// shading stage feeds normal maps downstream. So this table is built on the
// 8-bit *output* grid instead: for every level k it stores the smallest linear
// value that rounds to k, and byteFor() counts the thresholds v clears. That
// reproduces lround(255 * linearToSrgb(v)) by construction. Measured: 842 ms ->
// 193 ms with the output byte-identical.
struct SrgbTables
{
    // Sample resolution of the direct-lookup table. Over one step the output
    // byte can move by at most 12.92 / kSteps * 255 = 0.20 (12.92 is the
    // steepest slope of the transfer curve), so byteAt[] is never off by more
    // than one level and a single comparison settles it.
    static constexpr int kSteps = 1 << 14;

    double toLinear[256] = {};
    double threshold[256] = {};
    std::vector<unsigned char> byteAt;

    SrgbTables() : byteAt(kSteps + 1)
    {
        for (int i = 0; i < 256; ++i) {
            toLinear[i] = srgbToLinearExact(i / 255.0);
        }
        for (int i = 0; i <= kSteps; ++i) {
            byteAt[static_cast<size_t>(i)] = static_cast<unsigned char>(byteOfExact(static_cast<double>(i) / kSteps));
        }
        for (int k = 1; k < 256; ++k) {
            threshold[k] = solveThreshold(k);
        }
    }

    static int byteOfExact(double v)
    {
        return static_cast<int>(std::lround(linearToSrgbExact(v) * 255.0));
    }

    // Smallest linear value whose exact byte is k. 60 bisections over [0,1]
    // converges far past double precision; this runs 255 times, once.
    static double solveThreshold(int k)
    {
        double lo = 0.0;
        double hi = 1.0;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            if (byteOfExact(mid) >= k) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        return hi;
    }

    int byteFor(double v) const
    {
        if (v <= 0.0) {
            return 0;
        }
        if (v >= 1.0) {
            return 255;
        }
        const int i = static_cast<int>(v * kSteps);
        int k = byteAt[static_cast<size_t>(i)];
        if (k < 255 && v >= threshold[k + 1]) {
            ++k;
        }
        return k;
    }
};

const SrgbTables& srgbTables()
{
    // Thread-safe static initialisation; the ~4 ms of table building happens
    // once, on the first de-lighting pass.
    static const SrgbTables tables;
    return tables;
}

int wrapIndex(int index, int mod)
{
    const int v = index % mod;
    return v < 0 ? v + mod : v;
}

int scaledRadius(int baseRadius, int side)
{
    const double scale = static_cast<double>(side) / 1024.0;
    return std::max(1, static_cast<int>(std::lround(baseRadius * scale)));
}

struct Plane
{
    int w = 0;
    int h = 0;
    std::vector<float> v;

    Plane() = default;
    Plane(int width, int height)
        : w(width), h(height), v(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f)
    {
    }

    float at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
    float& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
};

// Separable sliding-window box mean, O(N) with O(width) working memory.
//
// Mirrors boxBlurGray() in TextureGenerator.cpp (same wrap-at-the-border
// semantics, so seamless results stay tileable) but works on the float planes
// this chain needs. An integral image is deliberately avoided here: a 4096^2
// image needs more than float precision for the running sum, and this filter
// runs dozens of times per image.
Plane boxMean(const Plane& src, int radius, bool wrap)
{
    if (radius <= 0 || src.w <= 0 || src.h <= 0) {
        return src;
    }

    // A wrapped window wider than the image would count pixels twice.
    const int limit = wrap ? std::max(0, (std::min(src.w, src.h) - 1) / 2) : radius;
    const int r = wrap ? std::min(radius, limit) : radius;
    if (r <= 0) {
        return src;
    }

    const double window = static_cast<double>(2 * r + 1);
    Plane horizontal(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        double sum = 0.0;
        for (int k = -r; k <= r; ++k) {
            sum += src.at(wrap ? wrapIndex(k, src.w) : std::clamp(k, 0, src.w - 1), y);
        }
        for (int x = 0; x < src.w; ++x) {
            horizontal.at(x, y) = static_cast<float>(sum / window);
            const int addX = x + r + 1;
            const int dropX = x - r;
            sum += src.at(wrap ? wrapIndex(addX, src.w) : std::clamp(addX, 0, src.w - 1), y);
            sum -= src.at(wrap ? wrapIndex(dropX, src.w) : std::clamp(dropX, 0, src.w - 1), y);
        }
    }

    Plane out(src.w, src.h);
    for (int x = 0; x < src.w; ++x) {
        double sum = 0.0;
        for (int k = -r; k <= r; ++k) {
            sum += horizontal.at(x, wrap ? wrapIndex(k, src.h) : std::clamp(k, 0, src.h - 1));
        }
        for (int y = 0; y < src.h; ++y) {
            out.at(x, y) = static_cast<float>(sum / window);
            const int addY = y + r + 1;
            const int dropY = y - r;
            sum += horizontal.at(x, wrap ? wrapIndex(addY, src.h) : std::clamp(addY, 0, src.h - 1));
            sum -= horizontal.at(x, wrap ? wrapIndex(dropY, src.h) : std::clamp(dropY, 0, src.h - 1));
        }
    }
    return out;
}

// He et al. guided filter. Edge-preserving smoother that costs the same as a
// box blur, which is what makes a multi-scale Retinex affordable at 4K.
Plane guidedFilter(const Plane& guide, const Plane& input, int radius, double eps, bool wrap)
{
    const size_t n = guide.v.size();
    const Plane meanGuide = boxMean(guide, radius, wrap);
    const Plane meanInput = boxMean(input, radius, wrap);

    Plane guideSq(guide.w, guide.h);
    Plane guideInput(guide.w, guide.h);
    for (size_t i = 0; i < n; ++i) {
        guideSq.v[i] = guide.v[i] * guide.v[i];
        guideInput.v[i] = guide.v[i] * input.v[i];
    }
    const Plane meanGuideSq = boxMean(guideSq, radius, wrap);
    const Plane meanGuideInput = boxMean(guideInput, radius, wrap);

    Plane a(guide.w, guide.h);
    Plane b(guide.w, guide.h);
    for (size_t i = 0; i < n; ++i) {
        const double mg = meanGuide.v[i];
        const double mi = meanInput.v[i];
        const double varGuide = static_cast<double>(meanGuideSq.v[i]) - mg * mg;
        const double covGuideInput = static_cast<double>(meanGuideInput.v[i]) - mg * mi;
        const double ai = covGuideInput / (varGuide + eps);
        a.v[i] = static_cast<float>(ai);
        b.v[i] = static_cast<float>(mi - ai * mg);
    }

    const Plane meanA = boxMean(a, radius, wrap);
    const Plane meanB = boxMean(b, radius, wrap);
    Plane out(guide.w, guide.h);
    for (size_t i = 0; i < n; ++i) {
        out.v[i] = static_cast<float>(static_cast<double>(meanA.v[i]) * guide.v[i] + meanB.v[i]);
    }
    return out;
}

// Sliding-window minimum or maximum along one axis, O(N) with a monotonic
// deque. Border handling mirrors boxMean() so seamless results stay tileable.
//
// The line is extended by the radius on both sides first, so the wrapped and
// the clamped case share one code path: with a folded index the deque's
// "indices only ever increase" invariant would break if we slid a window
// straight over the wrapped image.
Plane extreme1D(const Plane& src, int radius, bool wrap, bool wantMax, bool vertical)
{
    const int w = src.w;
    const int h = src.h;
    const int len = vertical ? h : w;
    const int other = vertical ? w : h;

    // A wrapped window wider than the line would count pixels twice.
    const int limit = wrap ? std::max(0, (len - 1) / 2) : radius;
    const int r = wrap ? std::min(radius, limit) : radius;
    if (r <= 0) {
        return src;
    }

    Plane out(w, h);
    const int ext = len + 2 * r;
    std::vector<float> line(static_cast<size_t>(ext));
    std::vector<int> dq(static_cast<size_t>(ext));

    for (int o = 0; o < other; ++o) {
        for (int j = 0; j < ext; ++j) {
            const int k = j - r;
            const int kk = wrap ? wrapIndex(k, len) : std::clamp(k, 0, len - 1);
            line[static_cast<size_t>(j)] = vertical ? src.at(o, kk) : src.at(kk, o);
        }

        int head = 0;
        int tail = 0;
        for (int j = 0; j < ext; ++j) {
            const float v = line[static_cast<size_t>(j)];
            while (tail > head) {
                const float back = line[static_cast<size_t>(dq[tail - 1])];
                if (wantMax ? back <= v : back >= v) {
                    --tail;
                } else {
                    break;
                }
            }
            dq[tail++] = j;
            const int drop = j - 2 * r;
            while (tail > head && dq[head] < drop) {
                ++head;
            }
            if (j >= 2 * r) {
                const int k = j - 2 * r;
                const float vv = line[static_cast<size_t>(dq[head])];
                if (vertical) {
                    out.at(o, k) = vv;
                } else {
                    out.at(k, o) = vv;
                }
            }
        }
    }
    return out;
}

// Stage 0: rebuild clipped highlights.
//
// removeSpecular() below can only *correct* a highlight, because it works by
// relighting the pixel's own chromaticity. A blown highlight has no
// chromaticity left to relight - it is r = g = b - so the guard in that
// function skips it, the pixel keeps its achromatic value, and once the
// shading stage has divided out its brightness it is left behind as a flat
// grey disc. Measured on a synthetic photo with a blown lobe, inside the lobe:
// saturation 0.050 where the surface underneath is 0.247.
//
// The information really is gone from those pixels and nothing recovers what
// the sensor never recorded. What is not gone is the surface around them, so
// the hole is regrown from its rim inward.
//
// Two failed attempts are worth recording, because both look reasonable and
// neither works:
//
//   - A wide-kernel average fill (normalised convolution over the hole)
//     reaches chroma 0.078 where the surfaces are 0.247. Still a grey disc,
//     just a softer one.
//   - A breadth-first fill that averages the neighbours it has already filled
//     reaches 0.094 and leaves a visible butterfly-shaped seam where the two
//     fronts meet.
//
// Both fail for the same reason: averaging chromaticity across a colour
// boundary produces a colour that exists nowhere on the surface. A highlight
// can straddle a boundary - the lobe in the test photo covers four tiles - and
// the average of four surface colours is exactly the grey we were trying to
// remove. So the fill copies instead of blending: every pixel in the hole
// takes the value of the nearest known pixel, which is what breadth-first
// order hands over for free, and each surface extends into the hole intact.
// The rule is that the fill may only ever *extend* colours that are already
// there, never invent one. It is worth more than it looks: the measured
// chroma barely moves (0.094 to 0.097), but a fill that has no wrong colours
// in it is one a later stage can build on.
//
// This runs *before* removeSpecular() on purpose. Once the hole has been
// refilled it is no longer brighter than its neighbourhood, so the highlight
// mask in that stage reads about zero there and leaves the repair alone;
// running it afterwards would have the two stages fighting over the same
// pixels.
// Illumination is a smooth field, and the nearest-pixel copy above is not.
//
// Copying luminance from the nearest known pixel is right for surface identity
// (chromaticity) and wrong for light: two rim pixels on opposite sides of a
// bright highlight can differ by a stop, and the copy puts a straight cliff
// where their two cells meet. That cliff is the faceted wedge that survives
// into the de-lit result - it is not a texture edge, it is where the fill
// stopped interpolating and started tiling.
//
// A diffuse illumination field has no such edges, so the field is solved for
// instead of copied: Laplace's equation over the hole, with the surrounding
// rim as the boundary condition, gives the smoothest field consistent with
// what the sensor did record. The two-pixel collar keeps the boundary values
// off the pixels the fill just wrote.
//
// Chromaticity gets the same treatment. Its nearest-donor cells meet along the
// same straight lines, and there the step shows up as a hue wedge rather than
// a brightness one - the pale triangle in the middle of a filled highlight.
// Interpolating it costs a muddy gradient between two hues where the surface
// really has an edge, but on the recorded side of that edge the fill cannot
// know where the edge is either, and a gradient is the honest answer.
void solveHoleField(Plane& field, const std::vector<uchar>& hole,
                    int w, int h, bool wrap, bool logDomain)
{
    const size_t n = field.v.size();
    std::vector<uchar> solve(hole);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uchar> next(solve);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (solve[i] != 0) {
                    continue;
                }
                const int up = wrap ? wrapIndex(y - 1, h) : y - 1;
                const int dn = wrap ? wrapIndex(y + 1, h) : y + 1;
                const int lf = wrap ? wrapIndex(x - 1, w) : x - 1;
                const int rt = wrap ? wrapIndex(x + 1, w) : x + 1;
                const bool touch =
                    (up >= 0 && up < h && solve[static_cast<size_t>(up) * w + x] != 0)
                    || (dn >= 0 && dn < h && solve[static_cast<size_t>(dn) * w + x] != 0)
                    || (lf >= 0 && lf < w && solve[i - static_cast<size_t>(x) + lf] != 0)
                    || (rt >= 0 && rt < w && solve[i - static_cast<size_t>(x) + rt] != 0);
                if (touch) {
                    next[i] = 1;
                }
            }
        }
        solve.swap(next);
    }

    // Luminance is solved in the log domain because light multiplies.
    // Chromaticity is solved as-is: it is a ratio, not a quantity, and a log
    // there would bias the interpolation toward the darker side.
    std::vector<float> value(n);
    for (size_t i = 0; i < n; ++i) {
        value[i] = logDomain ? std::log(std::max(field.v[i], 1e-6f)) : field.v[i];
    }

    // Successive over-relaxation. The region is wider than it is deep, so plain
    // Gauss-Seidel would need thousands of passes to flatten the middle; the
    // over-relaxation factor buys roughly an order of magnitude.
    //
    // The region is collected and its four neighbours resolved once: sweeping
    // the whole image 600 times would cost w*h per pass instead of the hole,
    // which on a 1024 px photo is a 30x waste for no benefit.
    std::vector<size_t> region;
    std::vector<int> upOf;
    std::vector<int> dnOf;
    std::vector<int> lfOf;
    std::vector<int> rtOf;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (solve[i] == 0) {
                continue;
            }
            const int up = wrap ? wrapIndex(y - 1, h) : y - 1;
            const int dn = wrap ? wrapIndex(y + 1, h) : y + 1;
            const int lf = wrap ? wrapIndex(x - 1, w) : x - 1;
            const int rt = wrap ? wrapIndex(x + 1, w) : x + 1;
            region.push_back(i);
            upOf.push_back(up >= 0 && up < h ? static_cast<int>(static_cast<size_t>(up) * w + x) : -1);
            dnOf.push_back(dn >= 0 && dn < h ? static_cast<int>(static_cast<size_t>(dn) * w + x) : -1);
            lfOf.push_back(lf >= 0 && lf < w ? static_cast<int>(i - static_cast<size_t>(x) + lf) : -1);
            rtOf.push_back(rt >= 0 && rt < w ? static_cast<int>(i - static_cast<size_t>(x) + rt) : -1);
        }
    }

    // SOR needs a number of passes on the order of the region's diameter: too
    // few and the middle of the hole keeps the tiled values it started from,
    // which is the artifact this exists to remove. Tying the count to the
    // diameter keeps a preview-sized hole proportionally cheap.
    int minX = w;
    int maxX = -1;
    int minY = h;
    int maxY = -1;
    for (const size_t i : region) {
        const int x = static_cast<int>(i % static_cast<size_t>(w));
        const int y = static_cast<int>(i / static_cast<size_t>(w));
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    const int extent = std::max(maxX - minX, maxY - minY) + 1;
    const int iterations = std::min(900, std::max(120, 3 * extent));

    const double omega = 1.85;
    const size_t count = region.size();
    for (int it = 0; it < iterations; ++it) {
        for (size_t k = 0; k < count; ++k) {
            double sum = 0.0;
            int neighbours = 0;
            if (upOf[k] >= 0) { sum += value[static_cast<size_t>(upOf[k])]; ++neighbours; }
            if (dnOf[k] >= 0) { sum += value[static_cast<size_t>(dnOf[k])]; ++neighbours; }
            if (lfOf[k] >= 0) { sum += value[static_cast<size_t>(lfOf[k])]; ++neighbours; }
            if (rtOf[k] >= 0) { sum += value[static_cast<size_t>(rtOf[k])]; ++neighbours; }
            if (neighbours == 0) {
                continue;
            }
            float& here = value[region[k]];
            const double target = sum / neighbours;
            here = static_cast<float>(here + omega * (target - static_cast<double>(here)));
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (solve[i] != 0) {
            field.v[i] = logDomain ? std::exp(value[i]) : value[i];
        }
    }
}

// 4-connected labelling of the blown-pixel mask. Returns a label per pixel
// (-1 where the pixel is known) and the area of every component in label
// order. Two separate questions need this: how big is the largest hole (which
// sizes the outward growth) and, per hole, whether the rim around it is one
// colour or several (which decides whether a filled chromaticity means
// anything).
std::vector<int> labelHoles(const std::vector<uchar>& hole, int w, int h, bool wrap,
                            std::vector<size_t>& areas)
{
    const size_t n = hole.size();
    std::vector<int> label(n, -1);
    std::vector<size_t> stack;
    areas.clear();
    for (size_t seed = 0; seed < n; ++seed) {
        if (hole[seed] == 0 || label[seed] >= 0) {
            continue;
        }
        const int id = static_cast<int>(areas.size());
        areas.push_back(0);
        stack.assign(1, seed);
        label[seed] = id;
        while (!stack.empty()) {
            const size_t p = stack.back();
            stack.pop_back();
            ++areas[static_cast<size_t>(id)];
            const int px = static_cast<int>(p % static_cast<size_t>(w));
            const int py = static_cast<int>(p / static_cast<size_t>(w));
            for (int k = 0; k < 4; ++k) {
                const int nx = px + (k == 0 ? 1 : (k == 1 ? -1 : 0));
                const int ny = py + (k == 2 ? 1 : (k == 3 ? -1 : 0));
                if (!wrap && (nx < 0 || ny < 0 || nx >= w || ny >= h)) {
                    continue;
                }
                const size_t q = static_cast<size_t>(wrap ? wrapIndex(ny, h) : ny) * w
                    + static_cast<size_t>(wrap ? wrapIndex(nx, w) : nx);
                if (hole[q] != 0 && label[q] < 0) {
                    label[q] = id;
                    stack.push_back(q);
                }
            }
        }
    }
    return label;
}

void rebuildClippedHighlights(Plane& red, Plane& green, Plane& blue, int side, bool wrap)
{
    const int w = red.w;
    const int h = red.h;
    const size_t n = red.v.size();

    Plane luma(w, h);
    for (size_t i = 0; i < n; ++i) {
        luma.v[i] = static_cast<float>(0.2126 * red.v[i] + 0.7152 * green.v[i] + 0.0722 * blue.v[i]);
    }

    // A blown pixel is one sitting at the image's own ceiling, not one above a
    // constant. The constant form silently disabled this whole stage on real
    // input: the seamless pass runs first and its low-frequency blend pulls a
    // 255 core down to 251, which is linear 0.955 and therefore below 0.985 on
    // every pixel. Measured on a photograph of tiles, the peak came back 251
    // and the stage returned before doing anything, leaving the highlight to be
    // turned into a flat grey disc by the shading division.
    double ceiling = 0.0;
    for (size_t i = 0; i < n; ++i) {
        ceiling = std::max(ceiling, static_cast<double>(std::min({red.v[i], green.v[i], blue.v[i]})));
    }
    const double clipLevel = std::min(0.985, ceiling - 0.005);

    std::vector<uchar> hole(n, 0);
    size_t holes = 0;
    for (size_t i = 0; i < n; ++i) {
        const double r = red.v[i];
        const double g = green.v[i];
        const double b = blue.v[i];
        if (r + g + b <= 1e-6) {
            continue;
        }
        // Seed: all three channels at the ceiling. That is a pixel whose
        // colour is genuinely gone, and removeSpecular() below cannot correct
        // it because there is no chromaticity left to relight.
        //
        // A pixel that is merely bright still has its chromaticity and is that
        // stage's business, not this one's. An earlier version also flagged
        // anything bright and low in saturation, which quietly fires on every
        // achromatic surface: a neutral grey texture has zero chroma
        // everywhere, so the whole image became one hole and the shading
        // gradient was flattened.
        if (std::min({r, g, b}) >= clipLevel) {
            hole[i] = 1;
            ++holes;
        }
    }
    if (std::getenv("TG_DELIGHT_DEBUG") != nullptr) {
        std::fprintf(stderr, "clip: ceiling=%.4f level=%.4f holes=%zu\n", ceiling, clipLevel,
                     holes);
    }
    if (holes == 0) {
        return;
    }

    // Grow the hole outward before filling, by a fraction of the hole's own
    // size rather than a fixed distance.
    //
    // The blown core is not the whole of it. Just outside the ceiling a pixel
    // is still nearly white, because the specular tail is still most of the
    // signal there, and rebuilding from that rim carries the wash inward
    // instead of removing it: on a synthetic scene with a strong lobe the ring
    // immediately outside the ceiling measured chroma 0.013 against a surface
    // chroma of 0.541, so extending from it recovered almost nothing. A
    // specular tail scales with the lobe that casts it, which is why the
    // distance here is tied to the hole rather than picked as a constant.
    //
    // Tying the growth to the hole is also what keeps this safe. It can only
    // ever expand outward from pixels that are already known to be destroyed,
    // so an image with no blown highlight is untouched by construction. The
    // alternative - growing wherever a pixel looks less colourful than its
    // neighbourhood - needs an absolute chroma threshold, and there is no
    // value of it that separates a washed-out highlight from a legitimately
    // pale tint.
    //
    // The 0.35 is measured, not chosen. Sweeping it on a 1024 px photo against
    // a known albedo, recovery keeps improving with more growth while the
    // error against the albedo turns around at 0.35:
    //
    //     factor   0.0     0.35    0.8     1.5     2.0
    //     psnr    19.12   19.53   18.76   18.20   18.17   (dB, highlight disc)
    //     chroma  0.141   0.175   0.240   0.297   0.314
    //
    // Chroma on its own would keep paying, because copying a more saturated
    // colour always raises it. The error turning over is what says the extra
    // growth is replacing real surface with a copy of the rim. Past the
    // turning point the two disagree, and the error is the one to trust.
    std::vector<int> label;
    std::vector<double> consistency;
    {
        // How big is "the hole"? It has to mean one hole, not the sum of all
        // of them. A single blown lobe is a compact disc, and its specular tail
        // extends about a third of its radius; a field of small blown specks -
        // say the near-white patches of a texture clipping under a strong lobe -
        // can have the same total area while each speck is a few pixels across.
        // Sizing the growth from the total area gave a 200 px radius on a
        // 1024 px image and asked for 70 px of growth on every speck, which
        // swallowed the centre of the picture; the fill then rebuilt it by
        // copying rim colours inward and the de-lit result scored 11 dB against
        // a 24 dB input. Taking the largest connected component instead keeps
        // the lobe case exactly as it was and stops the speck case from
        // growing past its own neighbours.
        std::vector<size_t> seedAreas;
        labelHoles(hole, w, h, wrap, seedAreas);
        size_t largestHole = 0;
        for (const size_t area : seedAreas) {
            largestHole = std::max(largestHole, area);
        }

        const double holeRadius = std::sqrt(static_cast<double>(largestHole) / 3.141592653589793);
        // A genuine lobe keeps the absolute floor that sheds its tail; a speck
        // does not, because four pixels of growth is wider than the speck.
        const int floorGrowth = holeRadius >= 24.0 ? scaledRadius(4, side) : 1;
        const int grow = std::max(floorGrowth, static_cast<int>(0.35 * holeRadius));
        Plane mask(w, h);
        for (size_t i = 0; i < n; ++i) {
            mask.v[i] = hole[i] != 0 ? 1.0f : 0.0f;
        }
        const Plane grown =
            extreme1D(extreme1D(mask, grow, wrap, true, false), grow, wrap, true, true);
        holes = 0;
        for (size_t i = 0; i < n; ++i) {
            hole[i] = grown.v[i] > 0.5f ? 1 : 0;
            holes += hole[i];
        }
    }

    // Label again: the growth has merged specks into the regions the fill will
    // actually see, and it is those regions whose rims matter.
    std::vector<size_t> areas;
    label = labelHoles(hole, w, h, wrap, areas);
    if (std::getenv("TG_DELIGHT_DEBUG") != nullptr) {
        std::fprintf(stderr, "clip: holes=%zu components=%zu top=", holes, areas.size());
        size_t top = 0;
        for (size_t a : areas) top = std::max(top, a);
        std::fprintf(stderr, "%zu\n", top);
    }

    // ---- Exemplar offsets -------------------------------------------------
    //
    // A nearest-donor fill is radial by construction, and radial is the wrong
    // shape for a surface that repeats. On a tiled photo the rays cut straight
    // across the grout, so the repair reads as a smear; on a synthetic
    // checkerboard it reads as a fan. Both are the same failure, and both go
    // away if the hole is filled by *translating* an intact part of the image
    // into it: the pattern continues, because it is the same pattern.
    //
    // The offset is found the only way that does not need a model of the
    // texture: shift the image by a candidate, and score how well its
    // high-passed luminance matches the high-passed luminance of the intact
    // ring around the hole. A repeating texture scores high at its period and
    // low everywhere else, and a photo with nothing repeating scores low
    // everywhere, which is exactly the case to fall back on the nearest-donor
    // fill. Measured: 0.86 on a 1024 tile photo at (0, 256), 0.998 on the
    // checkerboard scene at its 16 px period.
    //
    // The high-pass is what makes this work at all. Scoring raw pixels instead
    // picks whatever offset reproduces the highlight's own glow - the ring is
    // washed, so a washed piece of image matches it best - and on the tile
    // photo that returned (188, -108), a shift with no meaning that scored
    // better than the true 128 px tile period.
    Plane highPass(w, h);
    {
        const Plane wide = boxMean(luma, scaledRadius(8, side), wrap);
        for (size_t i = 0; i < n; ++i) {
            highPass.v[i] = luma.v[i] - wide.v[i];
        }
    }

    struct Exemplar
    {
        int dx = 0;
        int dy = 0;
        double score = 0.0;
        bool valid = false;
    };
    std::vector<Exemplar> exemplar(areas.size());

    const int searchRadius = std::max(16, std::min(static_cast<int>(side) / 4, 256));
    const int coarseStep = std::max(2, searchRadius / 32);

    for (size_t id = 0; id < areas.size(); ++id) {
        if (areas[id] < 200) {
            // A speck has no pattern to continue; the donors around it are
            // already the best available answer.
            continue;
        }
        std::vector<size_t> comp;
        comp.reserve(areas[id]);
        Plane only(w, h);
        for (size_t i = 0; i < n; ++i) {
            if (label[i] == static_cast<int>(id)) {
                only.v[i] = 1.0f;
                comp.push_back(i);
            }
        }
        if (comp.size() < 64) {
            continue;
        }
        // The ring the offset has to agree with: intact pixels just outside the
        // grown hole. Everything inside the hole is unusable as evidence.
        const Plane nearRing =
            extreme1D(extreme1D(only, 5, wrap, true, false), 5, wrap, true, true);
        std::vector<size_t> ring;
        for (size_t i = 0; i < n; ++i) {
            if (nearRing.v[i] > 0.5f && hole[i] == 0) {
                ring.push_back(i);
            }
        }
        if (ring.size() < 32) {
            continue;
        }
        auto sampleOf = [](const std::vector<size_t>& src) {
            std::vector<size_t> out;
            const size_t stride = std::max<size_t>(1, src.size() / 512);
            for (size_t k = 0; k < src.size(); k += stride) {
                out.push_back(src[k]);
            }
            return out;
        };
        const std::vector<size_t> ringSample = sampleOf(ring);
        const std::vector<size_t> compSample = sampleOf(comp);

        auto shifted = [&](size_t i, int dx, int dy, size_t& out) {
            const int x = static_cast<int>(i % static_cast<size_t>(w)) + dx;
            const int y = static_cast<int>(i / static_cast<size_t>(w)) + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) {
                return false;
            }
            out = static_cast<size_t>(y) * w + static_cast<size_t>(x);
            return true;
        };
        // A usable offset puts the whole hole and the whole ring on recorded
        // pixels - a fill sourced half from inside another blowout is no fill.
        auto usable = [&](int dx, int dy) {
            size_t j = 0;
            for (const size_t i : compSample) {
                if (!shifted(i, dx, dy, j) || hole[j] != 0) {
                    return false;
                }
            }
            for (const size_t i : ringSample) {
                if (!shifted(i, dx, dy, j) || hole[j] != 0) {
                    return false;
                }
            }
            return true;
        };
        auto scoreOf = [&](int dx, int dy, double& ncc, double& levelGap, double& hueGap) {
            double a[2] = {0.0, 0.0};
            double b[2] = {0.0, 0.0};
            double lumaA = 0.0;
            double lumaB = 0.0;
            double hueSum = 0.0;
            size_t hueCount = 0;
            size_t count = 0;
            size_t j = 0;
            for (const size_t i : ringSample) {
                if (!shifted(i, dx, dy, j)) {
                    return false;
                }
                const double av = highPass.v[i];
                const double bv = highPass.v[j];
                a[0] += av;
                a[1] += av * av;
                b[0] += bv;
                b[1] += bv * bv;
                lumaA += luma.v[i];
                lumaB += luma.v[j];
                // Where does the source sit in colour, against the ring it is
                // going to be pasted next to? Brightness and saturation both
                // lie here - the ring is washed by the very highlight being
                // repaired, so a clean source is always more colourful than the
                // ring it replaces, and penalising that would reject the good
                // offsets. Hue survives the wash, so only the hue is compared.
                const double sumI = static_cast<double>(red.v[i]) + green.v[i] + blue.v[i];
                const double sumJ = static_cast<double>(red.v[j]) + green.v[j] + blue.v[j];
                if (sumI > 1e-6 && sumJ > 1e-6) {
                    const double ir = red.v[i] / sumI;
                    const double ig = green.v[i] / sumI;
                    const double ib = blue.v[i] / sumI;
                    const double jr = red.v[j] / sumJ;
                    const double jg = green.v[j] / sumJ;
                    const double jb = blue.v[j] / sumJ;
                    const double spreadI =
                        std::max({ir, ig, ib}) - std::min({ir, ig, ib});
                    const double spreadJ =
                        std::max({jr, jg, jb}) - std::min({jr, jg, jb});
                    // A near-neutral pixel has no usable direction.
                    if (spreadI > 0.03 && spreadJ > 0.03) {
                        hueSum += 0.5 * (std::abs(ir - jr) + std::abs(ig - jg)
                                         + std::abs(ib - jb));
                        ++hueCount;
                    }
                }
                ++count;
            }
            if (count < 32) {
                return false;
            }
            const double m = static_cast<double>(count);
            const double av = a[0] / m;
            const double bv = b[0] / m;
            const double varA = std::max(0.0, a[1] / m - av * av);
            const double varB = std::max(0.0, b[1] / m - bv * bv);
            const double denom = std::sqrt(varA * varB);
            if (denom <= 1e-12) {
                return false;
            }
            double cov = 0.0;
            j = 0;
            for (const size_t i : ringSample) {
                shifted(i, dx, dy, j);
                cov += (highPass.v[i] - av) * (highPass.v[j] - bv);
            }
            cov /= m;
            ncc = cov / denom;
            levelGap = std::abs(lumaA - lumaB) / m;
            // Only meaningful when enough of the ring carries a hue at all;
            // otherwise the term is a measure of noise.
            hueGap = hueCount * 4 >= count
                ? hueSum / static_cast<double>(hueCount)
                : 0.0;
            return true;
        };

        struct Candidate
        {
            double scored = -2.0;
            double ncc = 0.0;
            double gap = 0.0;
            double hue = 0.0;
            int dx = 0;
            int dy = 0;
        };
        std::vector<Candidate> candidates;

        auto consider = [&](int dx, int dy) {
            if (std::abs(dx) < 4 && std::abs(dy) < 4) {
                return;
            }
            if (!usable(dx, dy)) {
                return;
            }
            Candidate c;
            c.dx = dx;
            c.dy = dy;
            if (!scoreOf(dx, dy, c.ncc, c.gap, c.hue)) {
                return;
            }
            // Hue only. Brightness and saturation are both *changed by the
            // very highlight being repaired*: the ring is washed, so it is
            // brighter and paler than the surface underneath, and a clean
            // source can never match it. Penalising that mismatch does not
            // find the surface, it finds another washed patch - measured on
            // the tile photo it chose (189, 0), which pasted a magenta tile
            // into an olive corner, over (0, 256), which is the tile period
            // and lines the grout up. Hue survives the wash, so hue is what
            // is compared.
            c.scored = c.ncc - 1.5 * c.hue;
            candidates.push_back(c);
        };
        // Ties are broken towards the shortest shift: a translation drags a
        // whole neighbourhood with it, so the longer it is the less the
        // surroundings have in common with the hole. Scores within 0.02 count
        // as tied - they all answer "the pattern repeats", which is the only
        // question this search can answer.
        auto bestOf = [](const std::vector<Candidate>& pool) {
            Candidate best;
            bool have = false;
            for (const Candidate& c : pool) {
                if (!have) {
                    best = c;
                    have = true;
                    continue;
                }
                const int magnitude = std::abs(c.dx) + std::abs(c.dy);
                const int bestMagnitude = std::abs(best.dx) + std::abs(best.dy);
                if (c.scored > best.scored + 0.02
                    || (c.scored > best.scored - 0.02 && magnitude < bestMagnitude)) {
                    best = c;
                }
            }
            return best;
        };

        for (int dy = -searchRadius; dy <= searchRadius; dy += coarseStep) {
            for (int dx = -searchRadius; dx <= searchRadius; dx += coarseStep) {
                consider(dx, dy);
            }
        }
        if (candidates.empty()) {
            continue;
        }
        Candidate best = bestOf(candidates);
        // Refine at pixel resolution around the coarse winner.
        if (coarseStep > 1) {
            for (int dy = best.dy - coarseStep; dy <= best.dy + coarseStep; ++dy) {
                for (int dx = best.dx - coarseStep; dx <= best.dx + coarseStep; ++dx) {
                    consider(dx, dy);
                }
            }
            best = bestOf(candidates);
        }
        // Below this the image is not repeating and the translation would be a
        // clone stamp of some unrelated surface.
        if (best.scored >= 0.55) {
            Exemplar chosen;
            chosen.dx = best.dx;
            chosen.dy = best.dy;
            chosen.score = best.ncc;
            chosen.valid = true;
            exemplar[id] = chosen;
        }
        if (std::getenv("TG_DELIGHT_DEBUG") != nullptr) {
            std::vector<Candidate> top(candidates);
            std::sort(top.begin(), top.end(), [](const Candidate& a, const Candidate& b) {
                return a.scored > b.scored;
            });
            const size_t show = std::min<size_t>(3, top.size());
            std::fprintf(stderr, "clip: exemplar id=%zu area=%zu", id, areas[id]);
            for (size_t k = 0; k < show; ++k) {
                std::fprintf(stderr, "  [(%d,%d) ncc=%.3f hue=%.3f s=%.3f]", top[k].dx, top[k].dy,
                             top[k].ncc, top[k].hue, top[k].scored);
            }
            std::fprintf(stderr, "  %s\n", best.scored >= 0.55 ? "used" : "rejected");
        }
    }

    auto neighbour = [&](size_t i, int k, size_t& out) {
        const int x = static_cast<int>(i % static_cast<size_t>(w));
        const int y = static_cast<int>(i / static_cast<size_t>(w));
        const int nx = x + (k == 0 ? 1 : (k == 1 ? -1 : 0));
        const int ny = y + (k == 2 ? 1 : (k == 3 ? -1 : 0));
        if (!wrap && (nx < 0 || ny < 0 || nx >= w || ny >= h)) {
            return false;
        }
        out = static_cast<size_t>(wrap ? wrapIndex(ny, h) : ny) * w
            + static_cast<size_t>(wrap ? wrapIndex(nx, w) : nx);
        return true;
    };

    // Chromaticity carries surface identity, luminance carries lighting. They
    // are grown separately so the fill cannot pair one surface's hue with
    // another's brightness.
    Plane chromaR(w, h);
    Plane chromaG(w, h);
    Plane chromaB(w, h);
    Plane fillLuma(w, h);
    std::vector<uchar> done(n, 0);
    std::vector<uchar> queued(n, 0);
    std::vector<int> from(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (hole[i] != 0) {
            continue;
        }
        const double sum = static_cast<double>(red.v[i]) + green.v[i] + blue.v[i];
        if (sum <= 1e-6) {
            continue;
        }
        chromaR.v[i] = static_cast<float>(red.v[i] / sum);
        chromaG.v[i] = static_cast<float>(green.v[i] / sum);
        chromaB.v[i] = static_cast<float>(blue.v[i] / sum);
        fillLuma.v[i] = luma.v[i];
        done[i] = 1;
    }

    // Translation fill, where the image repeats (see the offset search above).
    std::vector<uchar> fromExemplar(areas.size(), 0);
    for (size_t id = 0; id < exemplar.size(); ++id) {
        if (!exemplar[id].valid) {
            continue;
        }
        fromExemplar[id] = 1;
        for (size_t i = 0; i < n; ++i) {
            if (hole[i] == 0 || label[i] != static_cast<int>(id)) {
                continue;
            }
            const int x = static_cast<int>(i % static_cast<size_t>(w)) + exemplar[id].dx;
            const int y = static_cast<int>(i / static_cast<size_t>(w)) + exemplar[id].dy;
            if (x < 0 || y < 0 || x >= w || y >= h) {
                continue;
            }
            const size_t j = static_cast<size_t>(y) * w + static_cast<size_t>(x);
            if (hole[j] != 0 || done[j] == 0) {
                continue;
            }
            chromaR.v[i] = chromaR.v[j];
            chromaG.v[i] = chromaG.v[j];
            chromaB.v[i] = chromaB.v[j];
            fillLuma.v[i] = fillLuma.v[j];
            done[i] = 1;
        }
    }

    // A translated patch carries the *lighting* of where it came from, and the
    // ring around the hole carries the lighting of where it lands. When those
    // differ by a smooth amount - the bloom veil around a blowout is exactly
    // that - the copy lands as a darker blob with a hard edge, which is worse
    // than the flat disc it was meant to replace: on the user's tile photo the
    // patch came back at luma 121 against 138 on the ring twenty pixels away.
    // Level-match it in the gradient domain (Poisson): keep the patch's detail,
    // and solve for the offset field that makes its level meet the ring. The
    // boundary term is a log ratio because the patch is scaled by it.
    for (size_t id = 0; id < exemplar.size(); ++id) {
        if (!exemplar[id].valid) {
            continue;
        }
        const int dx = exemplar[id].dx;
        const int dy = exemplar[id].dy;
        std::vector<uchar> comp(n, 0);
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (hole[i] != 0 && label[i] == static_cast<int>(id)) {
                comp[i] = 1;
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        auto logLumaAt = [&](size_t i) {
            return std::log(std::max(static_cast<double>(luma.v[i]), 1e-4));
        };
        Plane bias(w, h);
        for (size_t i = 0; i < n; ++i) {
            if (hole[i] != 0) {
                continue;
            }
            const int x = static_cast<int>(i % static_cast<size_t>(w)) + dx;
            const int y = static_cast<int>(i / static_cast<size_t>(w)) + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) {
                continue;
            }
            const size_t j = static_cast<size_t>(y) * w + static_cast<size_t>(x);
            if (hole[j] != 0) {
                continue;
            }
            bias.v[i] = static_cast<float>(logLumaAt(i) - logLumaAt(j));
        }
        solveHoleField(bias, comp, w, h, wrap, false);
        for (size_t i = 0; i < n; ++i) {
            if (comp[i] == 0) {
                continue;
            }
            fillLuma.v[i] = static_cast<float>(
                static_cast<double>(fillLuma.v[i]) * std::exp(static_cast<double>(bias.v[i])));
        }
    }

    // Breadth-first from the rim inward. Two properties of this order are what
    // make the fill work: a pixel is enqueued by the first neighbour to reach
    // it, and because the queue is processed in distance order that neighbour
    // is the closest one. Recording it in `from` and copying from it is
    // therefore a nearest-known-pixel fill, with no blending anywhere.
    std::vector<size_t> queue;
    queue.reserve(holes);
    for (size_t i = 0; i < n; ++i) {
        if (hole[i] == 0 || done[i] != 0) {
            continue;
        }
        for (int k = 0; k < 4; ++k) {
            size_t j = 0;
            if (neighbour(i, k, j) && done[j] != 0) {
                queued[i] = 1;
                from[i] = static_cast<int>(j);
                queue.push_back(i);
                break;
            }
        }
    }

    for (size_t head = 0; head < queue.size(); ++head) {
        const size_t i = queue[head];
        if (from[i] >= 0) {
            const size_t p = static_cast<size_t>(from[i]);
            chromaR.v[i] = chromaR.v[p];
            chromaG.v[i] = chromaG.v[p];
            chromaB.v[i] = chromaB.v[p];
            fillLuma.v[i] = fillLuma.v[p];
        }
        done[i] = 1;
        for (int k = 0; k < 4; ++k) {
            size_t j = 0;
            if (neighbour(i, k, j) && hole[j] != 0 && queued[j] == 0 && done[j] == 0) {
                queued[j] = 1;
                from[j] = static_cast<int>(i);
                queue.push_back(j);
            }
        }
    }

    // How much of the filled colour does the rim actually support?
    //
    // The fill is a nearest-donor copy, so it is a continuation only when the
    // donors around a hole agree. That is exactly the property worth measuring:
    // walk the hole and count how often two neighbouring filled pixels ended up
    // with donors of different hue. A highlight over a surface - even one that
    // straddles two surfaces, warm on one side and cool on the other - has one
    // seam between donor regions and agrees everywhere else. A small blown
    // patch whose neighbours are red, green and blue has no interior at all,
    // only seams: the copy then tiles three hues into an eight-pixel square
    // and the Laplace solve blends them into (147,111,149) where the sensor's
    // own near-white reading was off by 23 levels a channel instead of 85.
    //
    // The disagreement is a fraction, so no absolute threshold on colour is
    // needed here - only a scale for "these two donors are different colours",
    // which is the same kind of number the rest of this pass already compares.
    consistency.assign(areas.size(), 1.0);
    if (!areas.empty()) {
        std::vector<size_t> pairs(areas.size(), 0);
        std::vector<size_t> clashes(areas.size(), 0);
        for (size_t i = 0; i < n; ++i) {
            if (hole[i] == 0 || label[i] < 0 || done[i] == 0) {
                continue;
            }
            const size_t id = static_cast<size_t>(label[i]);
            if (id >= areas.size()) {
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                size_t j = 0;
                if (!neighbour(i, k, j) || hole[j] == 0 || label[j] != label[i]) {
                    continue;
                }
                ++pairs[id];
                const double dr = std::abs(chromaR.v[i] - chromaR.v[j]);
                const double dg = std::abs(chromaG.v[i] - chromaG.v[j]);
                const double db = std::abs(chromaB.v[i] - chromaB.v[j]);
                if (std::max({dr, dg, db}) > 0.08) {
                    ++clashes[id];
                }
            }
        }
        for (size_t id = 0; id < areas.size(); ++id) {
            // Size decides first. A hole with an interior - the blown core of a
            // real highlight, a hundred pixels across - has room for each
            // surface around it to extend into, and the different hues then
            // meet along a few seams the way they do outside the hole. Only a
            // hole of a few pixels, with no interior at all, is the case where
            // extending turns into a confetti of several hues, and where the
            // recording is the better answer.
            if (areas[id] >= 200 || pairs[id] < 8) {
                continue;
            }
            const double clashRate = static_cast<double>(clashes[id]) / static_cast<double>(pairs[id]);
            // Both ends of the scale are measured on the test scenes rather
            // than picked: a highlight straddling warm and cool surfaces
            // clashes on 0.005 of its neighbouring pairs - one seam, which is
            // what a fill across two surfaces is supposed to produce - and a
            // blown white tile ringed by three hues clashes on 0.12 of them.
            // The first 0.02 is therefore a dead zone rather than evidence,
            // and the scale is spent between 0.02 and 0.12.
            const double clashExcess = std::max(0.0, clashRate - 0.02);
            consistency[id] = saturate(1.0 - clashExcess / 0.10);
        }
    }

    // The copied fields are the starting point, not the answer. A copy puts a
    // straight cliff wherever two donors' cells meet, and both fields show it
    // as a faceted wedge: luminance as a brightness step, chromaticity as a
    // hue step (the pale triangle the user circled). Both are solved for
    // instead, with the surrounding rim as the boundary condition, so the
    // filled area is the smoothest field consistent with what was recorded.
    // Only the small holes are smoothed. A few-pixel hole has no interior to
    // keep, and the seams where its donors meet are the entire artifact, so the
    // harmonic field is exactly right there. A large hole is the opposite case:
    // its interior is a surface, and flattening it to the harmonic field is
    // what turns the repair into a flat grey disc. Measured on a 1024 px tile
    // photo, a 70 px blown core grown by 35 px came back at chroma 0.05 against
    // 0.22 on the surrounding mosaic when the whole hole was solved, and kept
    // the mosaic's own patches when it was not. The 200 px cutoff is the same
    // one the consistency rule above uses for "this hole has an interior".
    std::vector<uchar> small(hole.size(), 0);
    bool anySmall = false;
    for (size_t i = 0; i < n; ++i) {
        if (hole[i] == 0 || label[i] < 0) {
            continue;
        }
        const size_t id = static_cast<size_t>(label[i]);
        if (id >= areas.size() || fromExemplar[id] != 0) {
            continue;
        }
        small[i] = 1;
        anySmall = true;
    }
    if (anySmall) {
        solveHoleField(fillLuma, small, w, h, wrap, true);
        solveHoleField(chromaR, small, w, h, wrap, false);
        solveHoleField(chromaG, small, w, h, wrap, false);
        solveHoleField(chromaB, small, w, h, wrap, false);
    }
    if (holes > 0) {
        // Laplace is linear, so a boundary that sums to one everywhere keeps
        // summing to one; renormalising only absorbs the residual of an
        // early-stopped SOR sweep.
        for (size_t i = 0; i < n; ++i) {
            if (hole[i] == 0) {
                continue;
            }
            const double sum = static_cast<double>(chromaR.v[i]) + chromaG.v[i] + chromaB.v[i];
            if (sum <= 1e-6) {
                chromaR.v[i] = 1.0f / 3.0f;
                chromaG.v[i] = 1.0f / 3.0f;
                chromaB.v[i] = 1.0f / 3.0f;
                continue;
            }
            chromaR.v[i] = static_cast<float>(chromaR.v[i] / sum);
            chromaG.v[i] = static_cast<float>(chromaG.v[i] / sum);
            chromaB.v[i] = static_cast<float>(chromaB.v[i] / sum);
        }
    }

    // Write the grown surface colour back over the hole.
    for (size_t i = 0; i < n; ++i) {
        if (hole[i] == 0) {
            continue;
        }
        const double cr = chromaR.v[i];
        const double cg = chromaG.v[i];
        const double cb = chromaB.v[i];
        const double asLuma = 0.2126 * cr + 0.7152 * cg + 0.0722 * cb;
        if (asLuma <= 1e-6) {
            continue;
        }
        // Scale the chromaticity so the rebuilt pixel carries the luminance of
        // the surrounding surface rather than the luminance of the blowout.
        const double total = static_cast<double>(fillLuma.v[i]) / asLuma;
        // How far to trust the reconstruction at all. When the whole rim is one
        // surface the fill is a continuation and is used in full. When the rim
        // disagrees - a blown patch whose neighbours are red, green and blue -
        // there is no surface to continue, and the blend of the rim hues is a
        // colour that is on nothing: on the coloured test scene a white patch
        // came back (147,111,149) where simply leaving the sensor's own
        // near-white reading would have been off by 23 levels a channel
        // instead of 85. Keeping the reading is the honest failure mode; only
        // the part of the fill the rim actually supports is applied.
        const double agree = (label[i] >= 0 && static_cast<size_t>(label[i]) < consistency.size())
            ? consistency[static_cast<size_t>(label[i])]
            : 1.0;
        red.v[i] = static_cast<float>(red.v[i] + (cr * total - red.v[i]) * agree);
        green.v[i] = static_cast<float>(green.v[i] + (cg * total - green.v[i]) * agree);
        blue.v[i] = static_cast<float>(blue.v[i] + (cb * total - blue.v[i]) * agree);
    }
}

// Stage 1: pull down specular highlights.
//
// Follows Yang et al. (CVPR 2010): the specular-free image I - min(R,G,B)
// cancels the achromatic specular component while keeping the diffuse
// chromaticity, so re-lighting that chromaticity with the surrounding diffuse
// intensity restores the surface colour underneath the highlight.
void removeSpecular(Plane& red, Plane& green, Plane& blue, int side, bool wrap, double amount)
{
    const size_t n = red.v.size();
    Plane luma(red.w, red.h);
    for (size_t i = 0; i < n; ++i) {
        luma.v[i] = 0.2126f * red.v[i] + 0.7152f * green.v[i] + 0.0722f * blue.v[i];
    }
    const Plane base = boxMean(luma, scaledRadius(24, side), wrap);

    Plane outR(red.w, red.h);
    Plane outG(red.w, red.h);
    Plane outB(red.w, red.h);
    for (size_t i = 0; i < n; ++i) {
        const double r = red.v[i];
        const double g = green.v[i];
        const double b = blue.v[i];

        // A pixel far brighter than its neighbourhood is a specular lobe. A
        // broad bright region has base ~= luma and leaves the mask at zero.
        const double baseL = std::max(static_cast<double>(base.v[i]), 1e-4);
        const double excess = std::max(0.0, static_cast<double>(luma.v[i]) - baseL);
        // A highlight is a *clear* excess, not any excess. A surface 18% above
        // its neighbourhood mean is ordinary texture - bright albedo next to
        // darker albedo - and rewriting it costs real colour: a green tile at
        // that level came back as (12,162,24) for an albedo of (26,184,36),
        // visibly more saturated than the material. Requiring half again over
        // the base before the rewrite starts keeps that tile intact and still
        // saturates on a genuine lobe.
        const double mask = saturate((excess / baseL - 0.5) * 2.0) * amount;

        double tR = r;
        double tG = g;
        double tB = b;
        if (mask > 0.0) {
            const double m = std::min({r, g, b});
            const double sfR = r - m;
            const double sfG = g - m;
            const double sfB = b - m;
            const double sfSum = sfR + sfG + sfB;
            // The specular-free image is I - min(R,G,B). Its whole job is to
            // cancel an achromatic addition and keep a *chromaticity*, so the
            // rewrite below is only meaningful when this pixel has one left.
            //
            // The failure this guards against is amplification, not a wrong
            // classification. On a slightly warm white (0.90, 0.88, 0.84 in
            // sRGB) the three differences are 0.115, 0.076 and 0; normalising
            // them is a gain of about 5x and hands back a fully saturated
            // orange. There is no threshold on that number that separates a
            // tinted white surface from a washed-out highlight - the two are
            // the same measurement - so instead the estimate fades in with its
            // own reliability. A red or green surface measures 0.6-0.8 here
            // and is rewritten outright; a white one measures 0.09 and keeps
            // its colour, which is the right answer: an achromatic surface
            // under an achromatic highlight has no hue to recover, and the
            // brightness is the shading stage's job.
            const double spread = sfSum / std::max(r + g + b, 1e-6);
            const double reliability = saturate((spread - 0.10) / 0.20);
            if (reliability > 0.0) {
                // Relight the surface colour with the *diffuse* intensity, not
                // with the neighbourhood's. The specular component is what
                // lifts this pixel above the local base, so removing it means
                // subtracting that excess from the pixel's own energy. Using
                // the base directly instead - as this used to - makes every
                // surface that is legitimately brighter than its surroundings
                // look like a highlight and repaints it by the neighbourhood's
                // brightness: a green tile 37% above the local mean came back
                // 1.7x too bright.
                const double intensity =
                    std::max(0.0, (r + g + b) - 3.0 * excess);
                const double weight = reliability * mask;
                tR = r + (sfR / sfSum * intensity - r) * weight;
                tG = g + (sfG / sfSum * intensity - g) * weight;
                tB = b + (sfB / sfSum * intensity - b) * weight;
                outR.v[i] = static_cast<float>(tR);
                outG.v[i] = static_cast<float>(tG);
                outB.v[i] = static_cast<float>(tB);
                continue;
            }
        }
        outR.v[i] = static_cast<float>(r + (tR - r) * mask);
        outG.v[i] = static_cast<float>(g + (tG - g) * mask);
        outB.v[i] = static_cast<float>(b + (tB - b) * mask);
    }
    red = outR;
    green = outG;
    blue = outB;
}

// Stage 2: neutralise a global illuminant colour cast (Shades-of-Gray, p = 6).
//
// Deliberately timid. A cast is worth removing, but a legitimately warm or cool
// material must survive, so the gains are clamped hard and applied at a half
// exponent. Anything more aggressive starts repainting people's materials.
void correctIlluminant(Plane& red, Plane& green, Plane& blue, double amount)
{
    const size_t n = red.v.size();
    if (n == 0) {
        return;
    }

    // Stride-sample: a global statistic does not need every pixel, and pow()
    // over 16M pixels three times over is seconds of work.
    const size_t stride = std::max<size_t>(1, n / 65536);
    constexpr double p = 6.0;
    double accR = 0.0;
    double accG = 0.0;
    double accB = 0.0;
    size_t counted = 0;
    for (size_t i = 0; i < n; i += stride) {
        accR += std::pow(static_cast<double>(red.v[i]), p);
        accG += std::pow(static_cast<double>(green.v[i]), p);
        accB += std::pow(static_cast<double>(blue.v[i]), p);
        ++counted;
    }
    if (counted == 0) {
        return;
    }

    const double mR = std::pow(accR / counted, 1.0 / p);
    const double mG = std::pow(accG / counted, 1.0 / p);
    const double mB = std::pow(accB / counted, 1.0 / p);
    const double target = (mR + mG + mB) / 3.0;
    if (target <= kEps) {
        return;
    }

    auto gainFor = [&](double m) {
        const double raw = target / std::max(m, kEps);
        return std::pow(std::clamp(raw, 0.85, 1.18), 0.5 * amount);
    };
    const double gR = gainFor(mR);
    const double gG = gainFor(mG);
    const double gB = gainFor(mB);

    for (size_t i = 0; i < n; ++i) {
        red.v[i] = static_cast<float>(red.v[i] * gR);
        green.v[i] = static_cast<float>(green.v[i] * gG);
        blue.v[i] = static_cast<float>(blue.v[i] * gB);
    }
}

// Stage 3: divide out the diffuse shading with a Retinex.
//
// The albedo is recovered as I * exp(-amount * (S - mean(S))). Subtracting the
// mean keeps the overall exposure where the photographer left it, and taking
// the scale factor from a zero-mean field means amount = 0 is the identity.
//
// This is deliberately *single* scale. Multi-scale Retinex (the MSR used for
// tone mapping) sums the detail bands, which double-counts fine detail and, at
// a band radius near the texture scale, deletes real albedo along with the
// shading. One radius comfortably larger than the texture works out as
// I / blur(I) = albedo / mean(albedo): the shading cancels and the texture
// survives. The trade-off is that shading *finer* than that radius is kept as
// albedo, which is the safe direction to err - it preserves detail.
void removeShading(Plane& red,
                   Plane& green,
                   Plane& blue,
                   const QImage& depth,
                   int side,
                   bool wrap,
                   double amount)
{
    const size_t n = red.v.size();
    Plane logLuma(red.w, red.h);
    for (size_t i = 0; i < n; ++i) {
        const double luma = 0.2126 * red.v[i] + 0.7152 * green.v[i] + 0.0722 * blue.v[i];
        logLuma.v[i] = static_cast<float>(std::log(std::max(luma, 1e-5)));
    }

    // Plain blur when we have nothing else: a Retinex illumination estimate has
    // to be smooth, so an edge-preserving filter guided by the *image* would be
    // exactly backwards - it would track the texture and leave it in place.
    //
    // The depth map is the one guide worth using, and it is used in place of
    // the image rather than alongside it: its discontinuities mark object
    // silhouettes, so keying on it keeps the estimate from bleeding a bright
    // surface onto a neighbouring dark one. Inside a surface depth is flat, the
    // filter's linear term collapses and it degrades to the plain blur.
    const int radius = scaledRadius(64, side);
    Plane shading(red.w, red.h);
    if (!depth.isNull() && depth.size() == QSize(red.w, red.h)) {
        const QImage d = depth.convertToFormat(QImage::Format_Grayscale8);
        Plane guide(red.w, red.h);
        for (int y = 0; y < red.h; ++y) {
            const uchar* line = d.constScanLine(y);
            for (int x = 0; x < red.w; ++x) {
                guide.at(x, y) = line[x] / 255.0f;
            }
        }
        shading = guidedFilter(guide, logLuma, radius, 1e-4, wrap);
    } else {
        shading = boxMean(logLuma, radius, wrap);
    }

    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mean += shading.v[i];
    }
    mean /= static_cast<double>(n);

    for (size_t i = 0; i < n; ++i) {
        const double scale = std::exp(-amount * (static_cast<double>(shading.v[i]) - mean));
        red.v[i] = static_cast<float>(red.v[i] * scale);
        green.v[i] = static_cast<float>(green.v[i] * scale);
        blue.v[i] = static_cast<float>(blue.v[i] * scale);
    }
}
} // namespace

namespace
{
// Shared body of both public entry points. `clipOnly` is what
// restoreClippedHighlights() uses to run the first stage alone, on the
// photograph and before any tiling has touched it.
QImage runStages(const QImage& image, const QImage& depth, double strength, bool wrap, bool clipOnly)
{
    // Byte-exact passthrough at the left end of the slider. The round trip
    // through linear light would otherwise introduce +/-1 rounding, and the
    // "strength 0 changes nothing" contract is worth more than the symmetry.
    if (image.isNull() || strength <= 0.0) {
        return image;
    }
    const double amount = saturate(strength);

    // Un-premultiply first: prepareSource() hands us ARGB32_Premultiplied, and
    // reading those bytes as colour would fold the alpha into the maths and
    // leave dark fringes around every transparent pixel.
    const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);
    const int w = rgba.width();
    const int h = rgba.height();
    if (w <= 0 || h <= 0) {
        return image;
    }

    const SrgbTables& tab = srgbTables();
    Plane red(w, h);
    Plane green(w, h);
    Plane blue(w, h);
    std::vector<uchar> alpha(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            const size_t i = static_cast<size_t>(y) * w + x;
            red.v[i] = static_cast<float>(tab.toLinear[qRed(px)]);
            green.v[i] = static_cast<float>(tab.toLinear[qGreen(px)]);
            blue.v[i] = static_cast<float>(tab.toLinear[qBlue(px)]);
            alpha[i] = static_cast<uchar>(qAlpha(px));
        }
    }

    const int side = std::max(w, h);
    // TG_DELIGHT_SKIP=csi h isolates a stage for diagnosis: the letters are
    // clipped highlights, specular, illuminant and shading. It is how the
    // regression on coloured albedo was traced to the first two stages rather
    // than to the shading estimate everyone suspected. Unset in normal runs.
    const QByteArray skipStages = qgetenv("TG_DELIGHT_SKIP");
    if (!skipStages.contains('c')) {
        rebuildClippedHighlights(red, green, blue, side, wrap);
    }
    if (!clipOnly && !skipStages.contains('s')) {
        removeSpecular(red, green, blue, side, wrap, amount);
    }
    if (!clipOnly && !skipStages.contains('i')) {
        correctIlluminant(red, green, blue, amount);
    }
    if (!clipOnly && !skipStages.contains('h')) {
        removeShading(red, green, blue, depth, side, wrap, amount);
    }

    QImage output(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const int a = alpha[i];
            const int r = tab.byteFor(red.v[i]);
            const int g = tab.byteFor(green.v[i]);
            const int b = tab.byteFor(blue.v[i]);
            // The buffer is premultiplied, so the stored bytes have to be too.
            line[x] = qRgba((r * a + 127) / 255, (g * a + 127) / 255, (b * a + 127) / 255, a);
        }
    }
    return output;
}
} // namespace

QImage LightingRemoval::remove(const QImage& image, const QImage& depth, double strength, bool wrap)
{
    return runStages(image, depth, strength, wrap, false);
}

QImage LightingRemoval::restoreClippedHighlights(const QImage& image)
{
    // Strength is fixed at 1: this stage has no slider, and a partial rebuild
    // would leave a highlight that is neither clipped nor recovered.
    return runStages(image, QImage(), 1.0, false, true);
}

int LightingRemoval::tableErrorLevels()
{
    const SrgbTables& tab = srgbTables();
    int worst = 0;

    // Decoding must be bit-exact: it is indexed by a byte, so every input the
    // pipeline can actually produce is covered by the table directly.
    for (int i = 0; i < 256; ++i) {
        if (tab.toLinear[i] != srgbToLinearExact(i / 255.0)) {
            return 1000; // not a rounding error; the table is wrong
        }
    }

    // Encoding takes an arbitrary float, so sample it densely and compare the
    // resulting byte level against the exact conversion.
    constexpr int kSamples = 1 << 20;
    for (int s = 0; s <= kSamples; ++s) {
        const double v = static_cast<double>(s) / kSamples;
        worst = std::max(worst, std::abs(tab.byteFor(v) - SrgbTables::byteOfExact(v)));
    }
    return worst;
}
