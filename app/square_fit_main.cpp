// What "Force 1:1" does to a non-square source.
//
// There are two defensible answers - crop the long edges away, or keep the
// whole picture and stretch it - and they are easy to swap by accident, because
// both produce an N x N image. The marker below is the difference: a red stripe
// on the far left of a 4:3 source survives a stretch and cannot survive a
// centre crop.
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    const QByteArray tail = detail.isEmpty() ? QByteArray() : (QStringLiteral("  ") + detail).toUtf8();
    std::printf("SQUARE %-36s %s%s\n", what, ok ? "PASS" : "FAIL", tail.constData());
    std::fflush(stdout);
    if (!ok) {
        ++failures;
    }
}

// 400 x 300, grey, with a red stripe down the left 20 columns.
QImage makeMarkedSource()
{
    QImage source(400, 300, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixel(x, y, x < 20 ? qRgb(220, 30, 30) : qRgb(120, 120, 120));
        }
    }
    return source;
}

// A source where the boring part and the interesting part are in different
// places, so what seam carving cuts is decided by the picture and not by where
// the code happens to start: flat grey on the left, grain on the right, and a
// dark square inside the grain to measure whether anything got scaled.
QImage makeCarveSource()
{
    QImage source(400, 300, QImage::Format_RGB32);
    unsigned state = 12345u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 16) & 0x3F);
    };
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 400; ++x) {
            int v = 128;
            if (x >= 120) {
                v = 100 + next() * 2;
            }
            if (x >= 240 && x < 300 && y >= 120 && y < 180) {
                v = 20;
            }
            source.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return source;
}

// Bounding box of the dark marker, in output pixels.
QRect darkBoundingBox(const QImage& image)
{
    int x0 = image.width();
    int y0 = image.height();
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) < 60) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

QImage baseColor(const QImage& source, SquareFit fit, bool square)
{
    TextureOptions options;
    options.resolution = 256;
    options.square = square;
    options.squareFit = fit;
    options.seamless = false;
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

bool stripePresent(const QImage& image)
{
    // The stripe is the only strongly red column in the source; after a stretch
    // it is still at the left edge, after a crop the left edge is grey.
    const QRgb pixel = image.pixel(image.width() / 40, image.height() / 2);
    return qRed(pixel) > 160 && qGreen(pixel) < 90;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QImage source = makeMarkedSource();
    std::printf("SQUARE source %dx%d\n", source.width(), source.height());

    {
        const QImage cropped = baseColor(source, SquareFit::Crop, true);
        check(cropped.width() == 256 && cropped.height() == 256, "crop: output is square",
              QStringLiteral("%1x%2").arg(cropped.width()).arg(cropped.height()));
        check(!stripePresent(cropped), "crop: the long edges are the part that goes");
    }
    {
        const QImage stretched = baseColor(source, SquareFit::Stretch, true);
        check(stretched.width() == 256 && stretched.height() == 256, "stretch: output is square",
              QStringLiteral("%1x%2").arg(stretched.width()).arg(stretched.height()));
        check(stripePresent(stretched), "stretch: the whole picture is kept");
    }
    // The three newer fits all keep the whole picture, so the stripe has to
    // survive each of them. They also differ in ways worth pinning down: the
    // mirror keeps the top edge exactly, the seam carve keeps the stripe at its
    // original width while removing columns somewhere else, and the
    // resynthesis has to put something plausible in the band it grew.
    {
        const QImage mirrored = baseColor(source, SquareFit::Mirror, true);
        check(mirrored.width() == 256 && mirrored.height() == 256, "mirror: output is square",
              QStringLiteral("%1x%2").arg(mirrored.width()).arg(mirrored.height()));
        check(stripePresent(mirrored), "mirror: the whole picture is kept");
        // A reflection makes the top row and the row at 2/3 of the height
        // identical when the source is 3:4 - that is the mirror axis landing
        // where the source's own bottom edge used to be.
        bool symmetric = true;
        for (int x = 0; x < mirrored.width(); x += 7) {
            const int a = mirrored.height() / 3;
            const int b = mirrored.height() - 1 - a;
            if (std::abs(qGray(mirrored.pixel(x, a)) - qGray(mirrored.pixel(x, b))) > 2) {
                symmetric = false;
                break;
            }
        }
        check(symmetric, "mirror: the reflection is symmetric about the centre");
    }
    {
        const QImage carvedSource = makeCarveSource();
        const QImage carved = baseColor(carvedSource, SquareFit::SeamCarve, true);
        check(carved.width() == 256 && carved.height() == 256, "seam carve: output is square",
              QStringLiteral("%1x%2").arg(carved.width()).arg(carved.height()));
        // The whole point of carving rather than scaling: the marker keeps its
        // shape. 100 columns have to go from a 400 px side; the flat band is
        // the only part with no energy, so all 100 come from there.
        const QRect box = darkBoundingBox(carved);
        const int w = box.width() + 1;
        const int h = box.height() + 1;
        check(!box.isNull() && std::abs(w - h) <= 3, "seam carve: the marker is not squashed",
              QStringLiteral("%1x%2").arg(w).arg(h));
        const QImage stretchedCarve = baseColor(carvedSource, SquareFit::Stretch, true);
        const QRect squashed = darkBoundingBox(stretchedCarve);
        check(!squashed.isNull() && std::abs((squashed.width() + 1) - (squashed.height() + 1)) > 8,
              "seam carve: ...and a stretch would have",
              QStringLiteral("stretch gives %1x%2").arg(squashed.width() + 1).arg(squashed.height() + 1));
    }
    {
        const QImage grown = baseColor(source, SquareFit::Resynthesize, true);
        check(grown.width() == 256 && grown.height() == 256, "resynthesize: output is square",
              QStringLiteral("%1x%2").arg(grown.width()).arg(grown.height()));
        check(stripePresent(grown), "resynthesize: the original is kept");
        // The bottom quarter is invented; it must not be a black or empty band.
        const QRgb invented = grown.pixel(grown.width() / 2, grown.height() - 8);
        check(qGray(invented) > 30, "resynthesize: the grown band is filled",
              QStringLiteral("grey %1").arg(qGray(invented)));
    }
    {
        const QImage native = baseColor(source, SquareFit::Crop, false);
        check(native.width() == 256 && native.height() == 192, "no 1:1 keeps the aspect ratio",
              QStringLiteral("%1x%2").arg(native.width()).arg(native.height()));
        check(stripePresent(native), "no 1:1 keeps the whole picture");
    }

    std::printf("SQUARE %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
