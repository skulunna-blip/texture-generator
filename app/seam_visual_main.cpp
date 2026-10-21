// Visual inspector for the seamless pass. Not an assertion - it writes PNGs so a
// human (or an image read) can look at what the seamless pass actually did.
//
//   tg_seam_visual <source.png> <out-dir> [crop-offset-px]
//
// The crop offset exists because a texture that already tiles proves nothing:
// shifting the crop makes the left and right edges disagree, which is the
// situation a photo is in. Each output is written both flat and as a 3x3 tiling,
// seamless off and on, so the seam (or its absence) is the only difference.
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QDir>
#include <QImage>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
QImage tile3x3(const QImage& image)
{
    QImage tiled(image.width() * 3, image.height() * 3, QImage::Format_ARGB32);
    tiled.fill(Qt::black);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            for (int sy = 0; sy < image.height(); ++sy) {
                const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(sy));
                QRgb* dst = reinterpret_cast<QRgb*>(tiled.scanLine(y * image.height() + sy)) + x * image.width();
                std::copy(row, row + image.width(), dst);
            }
        }
    }
    return tiled;
}

// Mean absolute difference between the left and right edge columns, and the top
// and bottom rows. A tiling texture has these at the same order as the interior
// neighbour difference; a broken one has them much larger.
double edgeDiff(const QImage& image)
{
    double total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        total += std::abs(qGray(image.pixel(0, y)) - qGray(image.pixel(image.width() - 1, y)));
    }
    return total / image.height();
}

double interiorDiff(const QImage& image)
{
    double total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        total += std::abs(qGray(image.pixel(image.width() / 2, y))
                          - qGray(image.pixel(image.width() / 2 - 1, y)));
    }
    return total / image.height();
}

QImage render(const QImage& source, bool seamless, SeamlessMode mode)
{
    TextureOptions options;
    options.resolution = 512;
    options.square = true;
    options.squareFit = SquareFit::Crop;
    options.seamless = seamless;
    options.seamlessMode = mode;
    options.backendMode = TextureBackendMode::ClassicFast;
    options.generateBaseColor = true;
    options.generateBump = false;
    options.generateNormal = false;
    options.generateRoughness = false;
    options.generateMetal = false;
    options.generateAo = false;
    options.generateAlpha = false;
    const TextureResult result = TextureGenerator::preview(source, options);
    return result.images.value(QStringLiteral("base"));
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::printf("usage: tg_seam_visual <source.png> <out-dir> [crop-offset-px]\n");
        return 2;
    }
    const QString sourcePath = QString::fromLocal8Bit(argv[1]);
    const QString outDir = QString::fromLocal8Bit(argv[2]);
    const int offset = argc > 3 ? std::atoi(argv[3]) : 0;

    QImage source(sourcePath);
    if (source.isNull()) {
        std::printf("SEAMVIS cannot read %s\n", sourcePath.toLocal8Bit().constData());
        return 2;
    }

    // Walk the crop window diagonally through an already-tileable texture. The
    // window keeps the size, so the content stays the same kind of thing while
    // the left and right edges stop agreeing - the photo situation.
    // Square window, slid by the offset and wrapped so any offset is legal on a
    // non-square source.
    const int side = std::min(source.width(), source.height());
    const int xoff = offset % (source.width() - side + 1);
    const int yoff = (offset / 2) % (source.height() - side + 1);
    const QImage crop = source.copy(QRect(xoff, yoff, side, side));
    QDir().mkpath(outDir);

    // Every seam engine, so the comparison the modes exist for can be looked
    // at rather than argued about.
    const struct
    {
        const char* label;
        bool seamless;
        SeamlessMode mode;
    } runs[] = {
        { "plain", false, SeamlessMode::Blend },
        { "seamless", true, SeamlessMode::Blend },
        { "patchmatch", true, SeamlessMode::PatchMatch },
        { "gpnn", true, SeamlessMode::Gpnn },
        { "resynth", true, SeamlessMode::Resynthesizer },
        { "mirror", true, SeamlessMode::Mirror },
    };
    for (const auto& run : runs) {
        const QImage out = render(crop, run.seamless, run.mode);
        const QString name = QString::fromLatin1(run.label);
        out.save(outDir + QLatin1Char('/') + name + QStringLiteral(".png"));
        tile3x3(out).save(outDir + QLatin1Char('/') + name + QStringLiteral("_tiled.png"));
        std::printf("SEAMVIS %-10s %dx%d edgeDiff=%5.2f interior=%5.2f\n",
                    run.label, out.width(), out.height(),
                    edgeDiff(out), interiorDiff(out));
        std::fflush(stdout);
    }
    return 0;
}
