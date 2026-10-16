// Regression check for the lighting-removal pass.
//
// Builds a synthetic scene whose albedo and shading are both known exactly
// (albedo = a neutral checkerboard with a tinted patch; shading = a broad
// multiplicative lobe plus a compact specular highlight), then verifies that
// remove() recovers something measurably closer to the albedo than the input
// photo was, without touching alpha and without breaking tiling.
#include "LightingRemoval.h"

#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
double srgbToLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double c)
{
    const double v = std::clamp(c, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

int encode(double linear)
{
    return static_cast<int>(std::lround(linearToSrgb(linear) * 255.0));
}

double luma(const QImage& image, int x, int y)
{
    const QRgb p = image.pixel(x, y);
    return 0.2126 * qRed(p) / 255.0 + 0.7152 * qGreen(p) / 255.0 + 0.0722 * qBlue(p) / 255.0;
}

double meanLuma(const QImage& image)
{
    double total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            total += luma(image, x, y);
        }
    }
    return total / (static_cast<double>(image.width()) * image.height());
}

// PSNR after matching mean luminance, so this measures recovered structure
// rather than the overall exposure (which the pass deliberately preserves).
double psnrMeanMatched(const QImage& a, const QImage& b)
{
    const double shift = meanLuma(a) - meanLuma(b);
    double sq = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const double d = luma(a, x, y) - std::clamp(luma(b, x, y) + shift, 0.0, 1.0);
            sq += d * d;
        }
    }
    const double mse = sq / (static_cast<double>(a.width()) * a.height());
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

double maxLumaIn(const QImage& image, int x0, int y0, int x1, int y1)
{
    double best = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            best = std::max(best, luma(image, x, y));
        }
    }
    return best;
}

struct Scene
{
    QImage albedo;
    QImage photo;
};

Scene makeScene(int size)
{
    Scene scene;
    scene.albedo = QImage(size, size, QImage::Format_RGB32);
    scene.photo = QImage(size, size, QImage::Format_RGB32);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Albedo: neutral checkerboard so the illuminant stage is a no-op,
            // plus a tinted patch so the chromaticity path still gets exercised.
            // 8 px tiles at 256 px is ~3% of the frame, about the scale of real
            // surface texture (weave, grain, speckle) relative to a photo.
            const double alb = (((x / 8) + (y / 8)) % 2 == 0) ? 0.62 : 0.38;
            double ar = alb;
            double ag = alb;
            double ab = alb;
            if (x > size * 3 / 4) {
                ar = alb * 1.05;
                ag = alb * 0.92;
                ab = alb * 0.85;
            }

            // Shading: a broad lobe plus a vertical falloff. Low frequency
            // relative to the 8 px checkerboard, as real diffuse light is.
            const double dx = (x - size * 0.3) / size;
            const double dy = (y - size * 0.3) / size;
            const double lobe = 1.0 + 0.85 * std::exp(-(dx * dx + dy * dy) * 6.0);
            const double shade = lobe * (1.0 - 0.25 * y / size);

            // A compact specular highlight: desaturated and much brighter than
            // its surroundings, which is the case the specular stage targets.
            const double spec = 1.6 * std::exp(-(dx * dx + dy * dy) * 900.0);

            scene.albedo.setPixel(x, y, qRgb(encode(srgbToLinear(ar)),
                                             encode(srgbToLinear(ag)),
                                             encode(srgbToLinear(ab))));
            scene.photo.setPixel(x, y, qRgb(encode(srgbToLinear(ar) * shade + spec),
                                            encode(srgbToLinear(ag) * shade + spec),
                                            encode(srgbToLinear(ab) * shade + spec)));
        }
    }
    return scene;
}

// Same metric as seam_check_main.cpp: mean absolute difference between opposite
// borders. A wrapped filter drives this down to the interior granularity.
double edgeDiff(const QImage& image)
{
    double total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        total += std::abs(luma(image, 0, y) - luma(image, image.width() - 1, y));
        total += std::abs(luma(image, y * image.width() / image.height(), 0)
                          - luma(image, y * image.width() / image.height(), image.height() - 1));
    }
    return total / (2.0 * image.height());
}

bool bytesEqual(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.format() != b.format()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        if (memcmp(a.constScanLine(y), b.constScanLine(y), static_cast<size_t>(a.width()) * 4) != 0) {
            return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    const int size = 256;
    const Scene scene = makeScene(size);

    // --- 0. The sRGB lookup tables are lossless ----------------------------
    // The transfer functions are 80% of this pass's cost, so they are table-
    // driven. This keeps proving the tables match the exact std::pow form
    // instead of trusting the comment that says they do.
    {
        const int err = LightingRemoval::tableErrorLevels();
        const bool ok = err == 0;
        std::printf("DELIGHT srgb_tables_exact    %s  worst deviation %d / 255\n", ok ? "PASS" : "FAIL", err);
        if (!ok) {
            ++failures;
        }
    }

    // --- 1. Identity at strength 0 -----------------------------------------
    {
        const QImage out = LightingRemoval::remove(scene.photo, QImage(), 0.0, false);
        const bool ok = bytesEqual(out, scene.photo);
        std::printf("DELIGHT identity_strength0  %s (byte-exact passthrough)\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            ++failures;
        }
    }

    // --- 2. Shading removal recovers the albedo ----------------------------
    {
        const QImage out = LightingRemoval::remove(scene.photo, QImage(), 1.0, false);
        const double before = psnrMeanMatched(scene.albedo, scene.photo);
        const double after = psnrMeanMatched(scene.albedo, out);
        const bool ok = after > before + 3.0;
        std::printf("DELIGHT shading_recovery     %s  psnr photo=%5.2f dB -> delit=%5.2f dB (gain %+5.2f)\n",
                    ok ? "PASS" : "FAIL", before, after, after - before);
        if (!ok) {
            ++failures;
        }
    }

    // --- 3. The specular highlight is pulled down --------------------------
    {
        const QImage out = LightingRemoval::remove(scene.photo, QImage(), 1.0, false);
        const int cx = static_cast<int>(size * 0.3);
        const double before = maxLumaIn(scene.photo, cx - 12, cx - 12, cx + 12, cx + 12);
        const double after = maxLumaIn(out, cx - 12, cx - 12, cx + 12, cx + 12);
        const bool ok = after < before - 0.05;
        std::printf("DELIGHT specular_suppressed %s  peak luma %5.3f -> %5.3f\n",
                    ok ? "PASS" : "FAIL", before, after);
        if (!ok) {
            ++failures;
        }
    }

    // --- 4. Alpha is carried through byte-exact ----------------------------
    {
        QImage src(size, size, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int a = (x * 255) / (size - 1);
                const int v = 40 + (y * 180) / (size - 1);
                src.setPixel(x, y, qRgba((v * a + 127) / 255, (v * a + 127) / 255, (v * a + 127) / 255, a));
            }
        }
        const QImage out = LightingRemoval::remove(src, QImage(), 1.0, false);
        int mismatches = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (qAlpha(out.pixel(x, y)) != qAlpha(src.pixel(x, y))) {
                    ++mismatches;
                }
            }
        }
        const bool ok = (mismatches == 0);
        std::printf("DELIGHT alpha_preserved      %s  %d mismatched pixels\n", ok ? "PASS" : "FAIL", mismatches);
        if (!ok) {
            ++failures;
        }
    }

    // --- 5. Seamless mode does not introduce a seam ------------------------
    {
        // De-lighting scales colour, it does not make a non-tiling photo tile.
        // The property worth testing is that wrap-aware filtering does not make
        // tiling *worse*: so feed it a source that already tiles (periodic in
        // both axes) and check the borders stay as continuous as they started.
        constexpr double kTwoPi = 6.283185307179586;
        QImage tileable(size, size, QImage::Format_RGB32);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const double u = kTwoPi * x / size;
                const double v = kTwoPi * y / size;
                const double g = 0.5 + 0.28 * std::sin(u) + 0.14 * std::sin(2.0 * v + 0.7);
                const int c = static_cast<int>(std::lround(std::clamp(g, 0.0, 1.0) * 255.0));
                tileable.setPixel(x, y, qRgb(c, c, c));
            }
        }
        const QImage wrapped = LightingRemoval::remove(tileable, QImage(), 0.8, true);
        const QImage clamped = LightingRemoval::remove(tileable, QImage(), 0.8, false);
        const double srcEdge = edgeDiff(tileable);
        const double wrapEdge = edgeDiff(wrapped);
        const double clampEdge = edgeDiff(clamped);
        const bool ok = wrapEdge <= srcEdge + 1.0 && wrapEdge < clampEdge;
        std::printf("DELIGHT seamless_wrap        %s  edge source=%5.2f wrap=%5.2f clamp=%5.2f\n",
                    ok ? "PASS" : "FAIL", srcEdge, wrapEdge, clampEdge);
        if (!ok) {
            ++failures;
        }
    }

    // --- 6. Depth-guided path runs and stays in range ----------------------
    {
        QImage depth(size, size, QImage::Format_Grayscale8);
        for (int y = 0; y < size; ++y) {
            uchar* line = depth.scanLine(y);
            for (int x = 0; x < size; ++x) {
                // A step edge down the middle plus a slope: geometry for the
                // guide to key on.
                line[x] = static_cast<uchar>(x < size / 2 ? 200 - y / 4 : 90 - y / 6);
            }
        }
        const QImage out = LightingRemoval::remove(scene.photo, depth, 1.0, false);
        int outOfRange = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const QRgb p = out.pixel(x, y);
                if (qRed(p) > 255 || qGreen(p) > 255 || qBlue(p) > 255) {
                    ++outOfRange;
                }
            }
        }
        const double psnr = psnrMeanMatched(scene.albedo, out);
        const bool ok = (outOfRange == 0) && (psnr > 8.0);
        std::printf("DELIGHT depth_guided         %s  psnr=%5.2f dB, out-of-range=%d\n",
                    ok ? "PASS" : "FAIL", psnr, outOfRange);
        if (!ok) {
            ++failures;
        }
    }

    // --- 7. A clipped highlight is rebuilt from the surrounding surfaces ----
    {
        // Two clearly different albedos meeting down the middle, under a blown
        // specular lobe centred exactly on the boundary. This is the case the
        // rebuild exists for: a blown pixel is r = g = b, so it carries no
        // colour at all and the specular stage cannot correct it, and the lobe
        // covers both surfaces at once so there is no single right answer to
        // average towards.
        //
        // Two things have to hold. The colour inside the blown region has to
        // come back, and it has to come back as *two different* colours. The
        // second is the one that matters: a fill that averaged across the
        // boundary could restore the saturation statistic while painting a
        // muddy colour that is on neither surface, which is what an earlier
        // version of this code did.
        //
        // What is deliberately *not* asserted here is that the rebuilt colour
        // matches the albedo closely. It cannot, on this scene. A pixel is
        // blown precisely when the specular is comparable to the diffuse, so
        // the ring immediately outside the ceiling is still washed out; a fill
        // that copies real neighbouring colour - which is the only kind that
        // does not invent colour - is bounded by how colourful that ring is.
        // Measured on this scene the ring encodes at chroma ~0.14 against an
        // albedo of 0.54, and the rebuild reaches 0.125, i.e. it is already at
        // the ceiling. The only way to print a higher number is to grow the
        // hole far enough to source from a more colourful ring, and that costs
        // real surface: sweeping the growth factor on a 1024 px photo, error
        // against the albedo bottoms out at the shipped value and rises on
        // both sides, while raw chroma keeps climbing. A threshold that can
        // only be met by making the result worse is measuring the wrong thing.
        //
        // So the assertions are: the case exists, colour comes back where
        // there was none, and the two surfaces stay apart. The last one is
        // what a mean fill cannot do - it collapses the separation to ~0.
        QImage albedo(size, size, QImage::Format_RGB32);
        QImage photo(size, size, QImage::Format_RGB32);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                // Saturated warm against saturated cool, so a blend between
                // them is unmistakable.
                const bool left = x < size / 2;
                const double ar = left ? 0.62 : 0.16;
                const double ag = left ? 0.20 : 0.30;
                const double ab = left ? 0.16 : 0.66;

                const double dx = (x - size * 0.5) / size;
                const double dy = (y - size * 0.5) / size;
                const double shade = 1.0 + 0.20 * std::exp(-(dx * dx + dy * dy) * 4.0);

                // Broad enough that the clipped core spans both halves.
                const double spec = 2.6 * std::exp(-(dx * dx + dy * dy) / 0.020)
                                  + 0.60 * std::exp(-(dx * dx + dy * dy) / 0.120);

                albedo.setPixel(x, y, qRgb(encode(srgbToLinear(ar)),
                                           encode(srgbToLinear(ag)),
                                           encode(srgbToLinear(ab))));
                photo.setPixel(x, y, qRgb(encode(srgbToLinear(ar) * shade + spec),
                                          encode(srgbToLinear(ag) * shade + spec),
                                          encode(srgbToLinear(ab) * shade + spec)));
            }
        }

        const QImage out = LightingRemoval::remove(photo, QImage(), 1.0, false);

        // The blown pixels, and only those: this is where the information was
        // destroyed, so it is the only fair place to judge the rebuild.
        auto chromaOf = [](const QImage& img, int x, int y) {
            const QRgb p = img.pixel(x, y);
            const double r = qRed(p) / 255.0;
            const double g = qGreen(p) / 255.0;
            const double b = qBlue(p) / 255.0;
            const double sum = r + g + b;
            return sum <= 1e-6 ? 0.0 : (sum - 3.0 * std::min({r, g, b})) / sum;
        };
        auto rednessOf = [](const QImage& img, int x, int y) {
            const QRgb p = img.pixel(x, y);
            const double sum = qRed(p) + qGreen(p) + qBlue(p);
            return sum <= 1e-6 ? 0.0 : qRed(p) / sum;
        };

        double albChroma = 0.0;
        double outChroma = 0.0;
        double albLeft = 0.0;
        double albRight = 0.0;
        double outLeft = 0.0;
        double outRight = 0.0;
        double phoChroma = 0.0;
        double phoLeft = 0.0;
        double phoRight = 0.0;
        int nLeft = 0;
        int nRight = 0;
        int blown = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const QRgb p = photo.pixel(x, y);
                if (qRed(p) < 255 && qGreen(p) < 255 && qBlue(p) < 255) {
                    continue;
                }
                ++blown;
                albChroma += chromaOf(albedo, x, y);
                outChroma += chromaOf(out, x, y);
                phoChroma += chromaOf(photo, x, y);
                if (x < size / 2) {
                    albLeft += rednessOf(albedo, x, y);
                    outLeft += rednessOf(out, x, y);
                    phoLeft += rednessOf(photo, x, y);
                    ++nLeft;
                } else {
                    albRight += rednessOf(albedo, x, y);
                    outRight += rednessOf(out, x, y);
                    phoRight += rednessOf(photo, x, y);
                    ++nRight;
                }
            }
        }
        const double albMean = albChroma / std::max(1, blown);
        const double outMean = outChroma / std::max(1, blown);
        const double chromaFrac = albMean > 1e-6 ? outMean / albMean : 0.0;

        albLeft /= std::max(1, nLeft);
        albRight /= std::max(1, nRight);
        outLeft /= std::max(1, nLeft);
        outRight /= std::max(1, nRight);
        phoLeft /= std::max(1, nLeft);
        phoRight /= std::max(1, nRight);
        phoChroma /= std::max(1, blown);
        const double albGap = std::abs(albLeft - albRight);
        const double outGap = std::abs(outLeft - outRight);
        const double gapFrac = albGap > 1e-6 ? outGap / albGap : 0.0;

        std::printf("DELIGHT clipped_trace        albedo chroma %.3f gap %.3f |"
                    " photo chroma %.3f gap %.3f | out chroma %.3f gap %.3f\n",
                    albMean, albGap, phoChroma, std::abs(phoLeft - phoRight), outMean, outGap);

        // The thresholds sit at about half the measured values, so ordinary
        // drift does not trip them but removing the rebuild (or the growth
        // that feeds it) does: with the growth disabled this scene measures
        // 0.080 / 0.066, and leaving the blown pixels alone measures 0.019 /
        // 0.017, which is the photo's own washed-out ring.
        const bool colourBack = chromaFrac >= 0.12;
        const bool surfacesApart = gapFrac >= 0.10;
        const bool ok = blown > 0 && colourBack && surfacesApart;
        std::printf("DELIGHT clipped_highlight    %s  %d blown px, chroma %.3f of albedo,"
                    " surface separation %.3f of albedo\n",
                    ok ? "PASS" : "FAIL", blown, chromaFrac, gapFrac);
        if (!ok) {
            ++failures;
        }
    }

    std::printf("DELIGHT %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED",
                failures, failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
