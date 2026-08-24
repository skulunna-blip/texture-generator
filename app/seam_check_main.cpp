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

    return 0;
}
