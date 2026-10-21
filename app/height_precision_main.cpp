// Regression check for the precision of the height pipeline.
//
// createHeightField is a chain of differences of blurs of one image, so every
// rounding in it is subtracted from another rounding rather than from the
// signal. When the four intermediate blurs were stored as Format_Grayscale8,
// that rounding was the whole of the normal map's detail on a smooth surface:
// the shipped normal tilted 0.26 deg on average while sitting 0.27 deg away
// from the smooth field it was supposed to describe. Rounding only the final
// output recovered none of it, because the four blurs are what lose the
// precision, not the export.
//
// The measurement below is the one that caught it. It is deliberately taken
// from the exported 8-bit normal map rather than from the internal field, so it
// tests what ships.
//
// The source is a low-frequency wave, which is the worst case: a smooth surface
// is where the relief is small enough for the rounding to be comparable to it.
// On a textured photo the same rounding is about 2% of the normal and this test
// would not see it - which is the honest scope of the bug, and the reason the
// second half of the test checks a textured source still keeps its relief.
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QImage>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr int kSize = 512;
// The mean angle by which the normal departs from its own low-pass. A correct
// pipeline describes a smooth surface smoothly; a pipeline that rounds its
// intermediate blurs describes it as a staircase.
//
// Measured on the 8-bit chain: 0.318 deg, reproduced by putting the two rounding
// sites back into this build. (The pre-change binary reads 0.373 on the same
// source; the gap is 8-bit values landing either side of a truncation boundary,
// not a structural difference - 99.8% of the exported bump pixels are identical
// between the two.) Measured on the float chain: 0.013 deg - a factor of 24.
// The threshold sits at 0.10, about 8x above the value it has to accept and 3x
// below the value it has to reject, so ordinary drift does not trip it but
// reintroducing an 8-bit intermediate does.
constexpr double kMaxRoughnessDeg = 0.10;
// A flat normal would also be perfectly smooth, so the relief has to be there
// too. The float chain measures 0.32 deg here.
constexpr double kMinTiltDeg = 0.15;

int failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    const QByteArray tail = detail.isEmpty() ? QByteArray() : (QStringLiteral("  ") + detail).toUtf8();
    std::printf("HEIGHT %-34s %s%s\n", what, ok ? "PASS" : "FAIL", tail.constData());
    std::fflush(stdout);
    if (!ok) {
        ++failures;
    }
}

double heightAt(double x, double y, double amplitude = 1.0)
{
    const double u = 2.0 * 3.141592653589793 * x / 128.0;
    const double v = 2.0 * 3.141592653589793 * y / 256.0;
    return 0.5 + amplitude * (0.22 * std::sin(u) + 0.12 * std::sin(v));
}

// Decodes the exported normal into unit vectors.
std::vector<double> decodeNormals(const QImage& normal, int& width, int& height)
{
    width = normal.width();
    height = normal.height();
    std::vector<double> out(static_cast<size_t>(width) * height * 3, 0.0);
    for (int y = 0; y < height; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(normal.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const double nx = qRed(line[x]) / 255.0 * 2.0 - 1.0;
            const double ny = qGreen(line[x]) / 255.0 * 2.0 - 1.0;
            const double nz = qBlue(line[x]) / 255.0 * 2.0 - 1.0;
            const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
            const size_t i = (static_cast<size_t>(y) * width + x) * 3;
            out[i] = nx / length;
            out[i + 1] = ny / length;
            out[i + 2] = nz / length;
        }
    }
    return out;
}

// Wrap-aware box blur of one component of a normal field.
std::vector<double> blurComponent(const std::vector<double>& field, int width, int height, int component, int radius)
{
    std::vector<double> plane(static_cast<size_t>(width) * height, 0.0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            plane[static_cast<size_t>(y) * width + x] = field[(static_cast<size_t>(y) * width + x) * 3 + component];
        }
    }

    const int stride = width + 1;
    std::vector<double> integral(static_cast<size_t>(stride) * (height + 1), 0.0);
    for (int y = 0; y < height; ++y) {
        double rowSum = 0.0;
        for (int x = 0; x < width; ++x) {
            rowSum += plane[static_cast<size_t>(y) * width + x];
            integral[static_cast<size_t>(y + 1) * stride + x + 1] =
                integral[static_cast<size_t>(y) * stride + x + 1] + rowSum;
        }
    }

    std::vector<double> out(static_cast<size_t>(width) * height, 0.0);
    for (int y = 0; y < height; ++y) {
        const int y0 = y - radius;
        const int y1 = y + radius;
        for (int x = 0; x < width; ++x) {
            const int x0 = x - radius;
            const int x1 = x + radius;
            const int area = (x1 - x0 + 1) * (y1 - y0 + 1);
            // The integral image is extended by one row/column of zeros on each
            // side by reading wrapped indices, so the border wraps rather than
            // clamping - the same convention the pipeline itself uses.
            auto at = [&](int ix, int iy) {
                const int wx = ((ix % width) + width) % width;
                const int wy = ((iy % height) + height) % height;
                return integral[static_cast<size_t>(wy + 1) * stride + wx + 1];
            };
            const double sum = at(x1, y1) - at(x0 - 1, y1) - at(x1, y0 - 1) + at(x0 - 1, y0 - 1);
            out[static_cast<size_t>(y) * width + x] = sum / area;
        }
    }
    return out;
}

double angleDeg(const std::vector<double>& a, const std::vector<double>& b, int width, int height, int margin)
{
    double total = 0.0;
    long count = 0;
    for (int y = margin; y < height - margin; ++y) {
        for (int x = margin; x < width - margin; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 3;
            double dot = 0.0;
            for (int c = 0; c < 3; ++c) {
                dot += a[i + c] * b[i + c];
            }
            total += std::acos(std::max(-1.0, std::min(1.0, dot))) * 180.0 / 3.141592653589793;
            ++count;
        }
    }
    return count > 0 ? total / count : 0.0;
}

double meanTiltDeg(const std::vector<double>& n, int width, int height, int margin)
{
    double total = 0.0;
    long count = 0;
    for (int y = margin; y < height - margin; ++y) {
        for (int x = margin; x < width - margin; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 3;
            total += std::acos(std::max(-1.0, std::min(1.0, n[i + 2]))) * 180.0 / 3.141592653589793;
            ++count;
        }
    }
    return count > 0 ? total / count : 0.0;
}

// Relief-carrying signal for the amplitude test. The smooth wave above sits at
// a wavelength the height field barely responds to, so its normal lands on the
// 8-bit export quantum (~0.2 deg) and two different amplitudes there would
// compare equal for the wrong reason. This one has wavelengths inside the
// bands the pipeline actually uses, so the tilt is degrees, not quantum.
double reliefAt(double x, double y, double amplitude)
{
    const double a = 2.0 * 3.141592653589793 * x / 24.0;
    const double b = 2.0 * 3.141592653589793 * y / 40.0;
    return 0.5 + amplitude * (0.22 * std::sin(a) + 0.15 * std::sin(b));
}

QImage makeReliefSource(double amplitude)
{
    constexpr int kSamples = 8;
    QImage source(kSize, kSize, QImage::Format_RGB32);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            double acc = 0.0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    acc += reliefAt(x + (sx + 0.5) / kSamples, y + (sy + 0.5) / kSamples, amplitude)
                         / double(kSamples * kSamples);
                }
            }
            const int c = static_cast<int>(std::lround(std::max(0.0, std::min(1.0, acc)) * 255.0));
            source.setPixel(x, y, qRgb(c, c, c));
        }
    }
    return source;
}

QImage makeSmoothSource(double amplitude = 1.0)
{
    // Box-filtered oversampling. Quantising the analytic wave straight to 8
    // bits leaves a +-0.5 level sawtooth on a slope of about two levels per
    // pixel, which is a 20% slope error at every pixel. That is source
    // quantisation, not pipeline rounding, and once the normal gain was made
    // strong enough to read on a real photo it dominated this measurement.
    // Averaging 64 analytic samples per pixel pushes it below what the
    // pipeline itself contributes, so the check measures the pipeline again.
    constexpr int kSamples = 8;
    QImage source(kSize, kSize, QImage::Format_RGB32);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            double acc = 0.0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    acc += heightAt(x + (sx + 0.5) / kSamples, y + (sy + 0.5) / kSamples, amplitude)
                         / double(kSamples * kSamples);
                }
            }
            const int c = static_cast<int>(std::lround(std::max(0.0, std::min(1.0, acc)) * 255.0));
            source.setPixel(x, y, qRgb(c, c, c));
        }
    }
    return source;
}

QImage makeTexturedSource()
{
    // Deterministic pseudo-texture: a handful of sinusoids plus two soft edges,
    // standing in for a photograph so the relief is large enough to be the
    // dominant signal rather than a rounding artefact.
    QImage source(kSize, kSize, QImage::Format_RGB32);
    unsigned int state = 12345u;
    auto nextRandom = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state % 100000u) / 100000.0;
    };
    struct Wave { double fx, fy, phase, weight; };
    std::vector<Wave> waves;
    for (int k = 1; k < 40; ++k) {
        waves.push_back({std::floor(nextRandom() * 59.0) + 1.0,
                         std::floor(nextRandom() * 59.0) + 1.0,
                         nextRandom() * 6.283185307179586,
                         1.0 / k});
    }
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            double value = 0.0;
            for (const Wave& w : waves) {
                value += w.weight * std::sin(2.0 * 3.141592653589793 * (w.fx * x + w.fy * y) / kSize + w.phase);
            }
            value /= 3.0;
            const double edges = 0.18 * std::tanh((std::sin(2.0 * 3.141592653589793 * x / 97.0)
                                                 + std::sin(2.0 * 3.141592653589793 * y / 141.0)) * 2.5);
            const double v = std::max(0.0, std::min(1.0, 0.5 + value + edges));
            const int c = static_cast<int>(std::lround(v * 255.0));
            source.setPixel(x, y, qRgb(c, c, c));
        }
    }
    return source;
}

struct NormalStats
{
    double roughness = 0.0; // deviation from its own low-pass, degrees
    double tilt = 0.0;      // mean tilt away from flat, degrees
    bool valid = false;
};

NormalStats measure(const QImage& source, TextureBackendMode backend, double bump = 1.0,
                    std::vector<double>* normalsOut = nullptr)
{
    TextureOptions options;
    options.resolution = kSize;
    options.square = true;
    options.seamless = true;
    options.bumpStrength = bump;
    options.normalStrength = 1.0;
    options.backendMode = backend;
    options.styleMode = StyleMode::Realistic;
    options.removeLighting = false;
    options.generateBaseColor = false;
    options.generateBump = false;
    options.generateNormal = true;
    options.generateRoughness = false;
    options.generateMetal = false;
    options.generateAo = false;
    options.generateAlpha = false;

    const TextureResult result = TextureGenerator::preview(source, options);
    const QImage normal = result.images.value(QStringLiteral("normal"));
    NormalStats stats;
    if (normal.isNull()) {
        return stats;
    }

    int width = 0;
    int height = 0;
    const std::vector<double> decoded = decodeNormals(normal, width, height);
    if (normalsOut) {
        *normalsOut = decoded;
    }
    std::vector<double> smooth = decoded;
    for (int c = 0; c < 3; ++c) {
        const std::vector<double> blurred = blurComponent(decoded, width, height, c, 16);
        for (size_t i = 0; i < blurred.size(); ++i) {
            smooth[i * 3 + c] = blurred[i];
        }
    }
    // Renormalise: the smoothed components are no longer unit length, and the
    // angle between two vectors does not care but acos does.
    for (size_t i = 0; i < smooth.size(); i += 3) {
        const double length = std::sqrt(smooth[i] * smooth[i] + smooth[i + 1] * smooth[i + 1] + smooth[i + 2] * smooth[i + 2]);
        if (length > 1e-9) {
            smooth[i] /= length;
            smooth[i + 1] /= length;
            smooth[i + 2] /= length;
        }
    }

    constexpr int kMargin = 40;
    stats.roughness = angleDeg(decoded, smooth, width, height, kMargin);
    stats.tilt = meanTiltDeg(decoded, width, height, kMargin);
    stats.valid = true;
    return stats;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // --- 1. A smooth surface gets a smooth normal --------------------------
    {
        // Measured at the lowest relief setting on purpose. The normal map
        // exports as 8-bit, so one step is about 0.2 degrees of tilt: at high
        // gain that quantum is itself the roughness being measured, and the
        // check would be reading the export format instead of the pipeline.
        // At the low end the quantum is far below the threshold, while the
        // staircase this guards against - 8-bit intermediate blurs - is a step
        // in the height field and shows up identically at any gain.
        const NormalStats stats = measure(makeSmoothSource(), TextureBackendMode::EnhancedDetail, 0.2);
        check(stats.valid, "smooth source produced a normal map");
        if (stats.valid) {
            check(stats.roughness <= kMaxRoughnessDeg, "smooth surface: normal is not a staircase",
                  QStringLiteral("deviation %1 deg, limit %2")
                        .arg(stats.roughness, 0, 'f', 3)
                        .arg(kMaxRoughnessDeg, 0, 'f', 2));
            check(stats.tilt >= kMinTiltDeg, "smooth surface: relief is still present",
                  QStringLiteral("tilt %1 deg, floor %2")
                        .arg(stats.tilt, 0, 'f', 3)
                        .arg(kMinTiltDeg, 0, 'f', 2));
        }
    }

    // --- 2. A textured source keeps its relief -----------------------------
    // The floor here is deliberately well below the measured value: this is
    // guarding against a fix that removes the artefact by removing the detail,
    // not pinning the exact number.
    //
    // Only the enhanced backend is checked above. The classic backend uses
    // smaller radii (2/6/12 rather than 3/8/20), so its bands are wider and the
    // rounding has less to subtract from - measured with the 8-bit blurs put
    // back, the classic path reads 0.041, which any threshold loose enough to
    // be safe would pass. An assertion that cannot fail is not a test, so it is
    // left out rather than included for the look of the thing.
    {
        const NormalStats stats = measure(makeTexturedSource(), TextureBackendMode::EnhancedDetail);
        check(stats.valid, "textured source produced a normal map");
        if (stats.valid) {
            check(stats.tilt >= 8.0, "textured source: relief survives",
                  QStringLiteral("tilt %1 deg, floor 8.0").arg(stats.tilt, 0, 'f', 3));
        }
    }

    // --- 3. The same surface at different contrast gives the same relief ---
    // This is what auto-levels in the height domain buys, and it is the check
    // the old slope-normalised gain would fail: that one could only boost a
    // low-contrast source by a fixed factor, so a third-amplitude copy of the
    // same wave came back with roughly a third of the tilt.
    {
        std::vector<double> full;
        std::vector<double> faint;
        const NormalStats a = measure(makeReliefSource(1.0), TextureBackendMode::EnhancedDetail, 1.0, &full);
        const NormalStats b = measure(makeReliefSource(0.35), TextureBackendMode::EnhancedDetail, 1.0, &faint);
        check(a.valid && b.valid, "both contrast variants produced a normal map");
        if (a.valid && b.valid) {
            const double angle = angleDeg(full, faint, kSize, kSize, 40);
            check(angle <= 3.0, "contrast does not change the relief",
                  QStringLiteral("full vs 35% amplitude differ by %1 deg, limit 3.0 "
                                 "(mean tilt %2 vs %3 deg)")
                        .arg(angle, 0, 'f', 2)
                        .arg(a.tilt, 0, 'f', 3)
                        .arg(b.tilt, 0, 'f', 3));
        }
    }

    // --- 4. The bump map is auto-levelled like the normal map --------------
    // The export used to be the raw height field, so a low-contrast photo gave
    // a low-contrast bump while the normal map derived from the same field had
    // already been stretched - the two disagreed about the same surface. The
    // floor is generous on purpose: this is checking that the stretch happens
    // at all, not pinning its exact strength.
    {
        TextureOptions options;
        options.resolution = kSize;
        options.square = true;
        options.seamless = true;
        options.bumpStrength = 1.0;
        options.backendMode = TextureBackendMode::EnhancedDetail;
        options.generateBaseColor = false;
        options.generateBump = true;
        options.generateNormal = false;
        options.generateRoughness = false;
        options.generateMetal = false;
        options.generateAo = false;
        options.generateAlpha = false;

        auto spread = [&](const QImage& source, double bumpStrength) {
            TextureOptions local = options;
            local.bumpStrength = bumpStrength;
            const QImage bump = TextureGenerator::preview(source, local)
                                    .images.value(QStringLiteral("bump"))
                                    .convertToFormat(QImage::Format_Grayscale8);
            if (bump.isNull()) {
                return -1.0;
            }
            double sum = 0.0;
            double sumSquares = 0.0;
            long count = 0;
            for (int y = 0; y < bump.height(); ++y) {
                const uchar* line = bump.constScanLine(y);
                for (int x = 0; x < bump.width(); ++x) {
                    sum += line[x];
                    sumSquares += static_cast<double>(line[x]) * line[x];
                    ++count;
                }
            }
            const double mean = sum / count;
            return std::sqrt(std::max(0.0, sumSquares / count - mean * mean));
        };

        const double full = spread(makeReliefSource(1.0), 1.0);
        const double faint = spread(makeReliefSource(0.35), 1.0);
        check(full > 0.0 && faint >= full * 0.8, "a faint source still fills the bump range",
              QStringLiteral("stddev %1 at full amplitude, %2 at 35%")
                  .arg(full, 0, 'f', 2)
                  .arg(faint, 0, 'f', 2));

        const double half = spread(makeReliefSource(1.0), 0.5);
        check(half > 0.0 && half < full * 0.8, "the relief slider still moves the bump",
              QStringLiteral("stddev %1 at gain 1.0, %2 at gain 0.5")
                  .arg(full, 0, 'f', 2)
                  .arg(half, 0, 'f', 2));
    }

    std::printf("HEIGHT %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
