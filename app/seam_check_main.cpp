// Seam check for the seamless pipeline: build a source image with a hard
// vertical seam (left half dark, right half bright), run TextureGenerator with
// seamless on/off, and measure how well the left and right edges of the output
// match after tiling. A tileable texture must satisfy out(0,y) ~= out(w-1,y)
// and out(x,0) ~= out(x,h-1) (equivalently, 3x3 tiling shows no visible seam).
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QImage>

#include <cmath>
#include <cstdio>

namespace
{
double edgeDiff(const QImage& image)
{
    if (image.isNull()) {
        return -1.0;
    }
    long total = 0;
    long count = 0;
    for (int y = 0; y < image.height(); ++y) {
        total += std::abs(qGray(image.pixel(0, y)) - qGray(image.pixel(image.width() - 1, y)));
        total += std::abs(qGray(image.pixel(y * image.width() / image.height(), 0)) - qGray(image.pixel(y * image.width() / image.height(), image.height() - 1)));
        count += 2;
    }
    return static_cast<double>(total) / count;
}

double interiorGranularity(const QImage& image)
{
    if (image.isNull()) {
        return -1.0;
    }
    long total = 0;
    long count = 0;
    for (int y = 1; y < image.height(); ++y) {
        for (int x = 1; x < image.width(); ++x) {
            total += std::abs(qGray(image.pixel(x, y)) - qGray(image.pixel(x - 1, y - 1)));
            ++count;
        }
    }
    return static_cast<double>(total) / count;
}

// Largest brightness step between neighbouring pixels, counting the wrap pair
// across the tile boundary. This is the number that sees a hard stitch: a mean
// over a whole edge (edgeDiff above) is diluted by the pixels that happened to
// match, while a single one-pixel cliff is what the eye actually finds.
double maxAdjacentStep(const QImage& image)
{
    if (image.isNull()) {
        return -1.0;
    }
    double worst = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        int previous = qGray(image.pixel(image.width() - 1, y));
        for (int x = 0; x < image.width(); ++x) {
            const int current = qGray(image.pixel(x, y));
            worst = std::max(worst, static_cast<double>(std::abs(current - previous)));
            previous = current;
        }
    }
    return worst;
}

QImage seamSource(int size)
{
    // Continuous in the middle, but a hard jump between the left and right
    // edges (and top/bottom): exactly the "doesn't tile" case a seamless pass
    // is supposed to fix. Offset-blend samples its edges from the middle of the
    // source, so the seam must sit at the border for the metric to be honest.
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int v = (x * 220) / size + ((y * 60) / size);
            image.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return image;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QImage source = seamSource(256);

    TextureOptions opts;
    opts.resolution = 512;
    opts.square = true;
    opts.generateBump = true;
    opts.generateNormal = true;
    opts.generateRoughness = true;
    opts.generateMetal = true;
    opts.generateAo = true;
    opts.generateAlpha = false;

    for (bool seamless : {false, true}) {
        opts.seamless = seamless;
        const TextureResult result = TextureGenerator::preview(source, opts);
        std::printf("SEAM seamless=%d\n", seamless ? 1 : 0);
        for (const char* key : {"base", "bump", "normal", "roughness", "metal", "ao"}) {
            const QImage map = result.images.value(QString::fromLatin1(key));
            std::printf("  %-10s edgeDiff=%6.2f interior=%6.2f\n",
                        key, edgeDiff(map), interiorGranularity(map));
        }
        std::fflush(stdout);
    }

    // --- The seam the edge metric cannot see -------------------------------
    //
    // Everything above only looks at the tile border, and a pass can leave the
    // border perfect while tearing the inside: stitching two half-shifted
    // copies with a hard line moves the seam from the edge of the tile into the
    // middle of it. That is exactly what this pipeline used to do, and the edge
    // numbers were excellent while the output was a 2x2 checkerboard.
    //
    // A pure gradient is the case that exposes it. There is no detail to hide
    // behind, so any hard stitch shows up as a step half the height of the
    // gradient, while a correct pass leaves nothing but a gentle ramp.
    QImage gradient(256, 256, QImage::Format_RGB32);
    for (int y = 0; y < gradient.height(); ++y) {
        for (int x = 0; x < gradient.width(); ++x) {
            const int v = 60 + (160 * x) / (gradient.width() - 1);
            gradient.setPixel(x, y, qRgb(v, v, v));
        }
    }

    TextureOptions flatOptions;
    flatOptions.resolution = 256;
    flatOptions.seamless = true;
    flatOptions.backendMode = TextureBackendMode::ClassicFast;
    flatOptions.generateBump = false;
    flatOptions.generateNormal = false;
    flatOptions.generateRoughness = false;
    flatOptions.generateMetal = false;
    flatOptions.generateAo = false;
    flatOptions.generateAlpha = false;
    const QImage flat = TextureGenerator::preview(gradient, flatOptions).images.value(QStringLiteral("base"));

    // The source steps by 0.63 levels per pixel, and a correct pass measures
    // about 4 here (the coarse ramp plus resampling). A hard stitch measures
    // half the gradient, around 80 - there is no ambiguity between the two.
    const double step = maxAdjacentStep(flat);
    const double border = edgeDiff(flat);
    std::printf("SEAM gradient   maxStep=%6.2f  border=%6.2f\n", step, border);
    std::fflush(stdout);

    int failures = 0;
    auto check = [&](bool ok, const char* what, const QString& detail) {
        std::printf("SEAM %-44s %s  %s\n", what, ok ? "PASS" : "FAIL", detail.toUtf8().constData());
        std::fflush(stdout);
        if (!ok) {
            ++failures;
        }
    };
    check(step < 6.0, "a smooth source stays smooth inside the tile",
          QStringLiteral("max adjacent step %1 (source steps 0.63)").arg(step, 0, 'f', 2));
    check(border < 4.0, "and it still tiles",
          QStringLiteral("border difference %1").arg(border, 0, 'f', 2));

    std::printf("SEAM %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
