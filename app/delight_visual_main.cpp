// Visual inspector for the de-lighting pass, the counterpart of
// tg_seam_visual. Not an assertion: it runs the real LightingRemoval on a
// photograph and writes the result next to the input, so a complaint about
// "the result still looks wrong" can be answered by looking at the same
// pixels rather than by reading a PSNR.
//
// Usage: tg_delight_visual <source.png> <out-dir> [strength] [seamless]
//
// Optional depth is deliberately not used: this tool is about the classic
// path, and the depth model changes what the shading estimate is allowed to
// follow rather than whether it runs.
#include "LightingRemoval.h"
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// Mean absolute difference between opposite borders. A wrapped pass drives it
// down to the interior granularity; this only prints, it does not assert.
double edgeDiff(const QImage& image)
{
    if (image.isNull()) {
        return -1.0;
    }
    long total = 0;
    long count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const int a = image.pixel(0, y);
        const int b = image.pixel(image.width() - 1, y);
        total += std::abs(qGray(a) - qGray(b));
        ++count;
    }
    for (int x = 0; x < image.width(); ++x) {
        const int a = image.pixel(x, 0);
        const int b = image.pixel(x, image.height() - 1);
        total += std::abs(qGray(a) - qGray(b));
        ++count;
    }
    return count == 0 ? -1.0 : static_cast<double>(total) / count;
}

QImage tile3x3(const QImage& image)
{
    if (image.isNull()) {
        return image;
    }
    QImage out(image.width() * 3, image.height() * 3, QImage::Format_RGB32);
    for (int ty = 0; ty < 3; ++ty) {
        for (int tx = 0; tx < 3; ++tx) {
            for (int y = 0; y < image.height(); ++y) {
                std::memcpy(out.scanLine(ty * image.height() + y) + tx * image.width() * 4,
                            image.constScanLine(y),
                            static_cast<size_t>(image.width()) * 4);
            }
        }
    }
    return out;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::printf("usage: tg_delight_visual <source.png> <out-dir> [strength] [seamless]\n");
        return 2;
    }

    const QString sourcePath = QString::fromLocal8Bit(argv[1]);
    const QString outDir = QString::fromLocal8Bit(argv[2]);
    const double strength = argc > 3 ? std::atof(argv[3]) : 1.0;
    const bool seamless = argc > 4 && std::atoi(argv[4]) != 0;

    QImage source(sourcePath);
    if (source.isNull()) {
        std::fprintf(stderr, "cannot read %s\n", sourcePath.toLocal8Bit().constData());
        return 1;
    }
    source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QDir().mkpath(outDir);

    // Every image here comes out of preview() so it is the pipeline the app
    // actually runs, not a re-implementation of it. The derivative maps are
    // switched off to keep the run cheap.
    TextureOptions options;
    options.seamless = seamless;
    options.square = true;
    options.resolution = std::min(4096, std::max(source.width(), source.height()));
    options.generateBump = false;
    options.generateNormal = false;
    options.generateRoughness = false;
    options.generateMetal = false;
    options.generateAo = false;
    options.generateAlpha = false;
    options.removeLighting = true;
    options.delightStrength = strength;

    // The intermediate the highlight rebuild sees: cropped and scaled, but not
    // yet tiled and not yet de-lit.
    options.seamless = false;
    options.removeLighting = false;
    const QImage untiled =
        TextureGenerator::preview(source, options).images.value(QStringLiteral("base"));
    const QImage repaired = LightingRemoval::restoreClippedHighlights(untiled);

    // The real thing, in the app's order: rebuild on the photo, then tile.
    options.seamless = seamless;
    options.removeLighting = true;
    const QImage plain =
        TextureGenerator::preview(source, options).images.value(QStringLiteral("base"));
    options.delightStrength = std::min(1.0, strength * 2.0);
    const QImage doubled =
        TextureGenerator::preview(source, options).images.value(QStringLiteral("base"));

    untiled.save(outDir + QStringLiteral("/prepared.png"));
    repaired.save(outDir + QStringLiteral("/cliprepair.png"));
    plain.save(outDir + QStringLiteral("/delit.png"));
    doubled.save(outDir + QStringLiteral("/delit_2x.png"));
    tile3x3(plain).save(outDir + QStringLiteral("/delit_tiled.png"));

    std::printf("DELIGHTVIS %dx%d strength=%.2f seamless=%d\n", plain.width(), plain.height(),
                strength, seamless ? 1 : 0);
    std::printf("  untiled    edgeDiff=%6.2f\n", edgeDiff(untiled));
    std::printf("  cliprepair edgeDiff=%6.2f\n", edgeDiff(repaired));
    std::printf("  delit      edgeDiff=%6.2f\n", edgeDiff(plain));
    std::printf("  wrote %s/{prepared,delit,delit_2x,delit_tiled}.png\n",
                outDir.toLocal8Bit().constData());
    return 0;
}
