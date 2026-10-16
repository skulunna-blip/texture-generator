#include "LightingRemoval.h"

#include <algorithm>
#include <cmath>
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
void rebuildClippedHighlights(Plane& red, Plane& green, Plane& blue, int side, bool wrap)
{
    const int w = red.w;
    const int h = red.h;
    const size_t n = red.v.size();

    Plane luma(w, h);
    for (size_t i = 0; i < n; ++i) {
        luma.v[i] = static_cast<float>(0.2126 * red.v[i] + 0.7152 * green.v[i] + 0.0722 * blue.v[i]);
    }

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
        if (std::min({r, g, b}) >= 0.985) {
            hole[i] = 1;
            ++holes;
        }
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
    {
        const double holeRadius = std::sqrt(static_cast<double>(holes) / 3.141592653589793);
        const int grow = std::max(scaledRadius(4, side),
                                  static_cast<int>(0.35 * holeRadius));
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

    // Breadth-first from the rim inward. Two properties of this order are what
    // make the fill work: a pixel is enqueued by the first neighbour to reach
    // it, and because the queue is processed in distance order that neighbour
    // is the closest one. Recording it in `from` and copying from it is
    // therefore a nearest-known-pixel fill, with no blending anywhere.
    std::vector<size_t> queue;
    queue.reserve(holes);
    for (size_t i = 0; i < n; ++i) {
        if (hole[i] == 0) {
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
            if (neighbour(i, k, j) && hole[j] != 0 && queued[j] == 0) {
                queued[j] = 1;
                from[j] = static_cast<int>(i);
                queue.push_back(j);
            }
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
        red.v[i] = static_cast<float>(cr * total);
        green.v[i] = static_cast<float>(cg * total);
        blue.v[i] = static_cast<float>(cb * total);
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
        const double mask = saturate((luma.v[i] - baseL) / baseL * 2.0) * amount;

        double tR = r;
        double tG = g;
        double tB = b;
        if (mask > 0.0) {
            const double m = std::min({r, g, b});
            const double sfR = r - m;
            const double sfG = g - m;
            const double sfB = b - m;
            const double sfSum = sfR + sfG + sfB;
            if (sfSum > 1e-5) {
                const double intensity = 3.0 * baseL;
                tR = sfR / sfSum * intensity;
                tG = sfG / sfSum * intensity;
                tB = sfB / sfSum * intensity;
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

QImage LightingRemoval::remove(const QImage& image, const QImage& depth, double strength, bool wrap)
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
    rebuildClippedHighlights(red, green, blue, side, wrap);
    removeSpecular(red, green, blue, side, wrap, amount);
    correctIlluminant(red, green, blue, amount);
    removeShading(red, green, blue, depth, side, wrap, amount);

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
