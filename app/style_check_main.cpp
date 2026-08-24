// Anime style check: run TextureGenerator in both style modes on a synthetic
// input with a strong gradient edge, then assert the anime output actually
// becomes cell-shaded:
//   - far fewer unique colors (posterized flat tints) than realistic input;
//   - a dark outline sits on the strong edge (luma at the edge drops);
//   - hue is preserved (red region stays red).
// Also verifies the anime roughness map is pulled toward smooth.
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QImage>
#include <QSet>

#include <cstdio>
#include <cmath>

namespace
{
QImage gradientSource(int size)
{
    // Clean synthetic material: bright red top half, deep blue bottom half,
    // hard horizontal edge in the middle, plus a subtle gradient inside each
    // half so posterization has something to collapse.
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool top = y < size / 2;
            const int shade = static_cast<int>((x * 90.0) / size);
            if (top) {
                image.setPixel(x, y, qRgb(210 + shade / 3, 40 + shade / 5, 40 + shade / 8));
            } else {
                image.setPixel(x, y, qRgb(40 + shade / 8, 70 + shade / 5, 210 + shade / 3));
            }
        }
    }
    return image;
}

int uniqueColorCount(const QImage& image)
{
    QSet<QRgb> seen;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            seen.insert(image.pixel(x, y));
        }
    }
    return seen.size();
}

double regionGray(const QImage& image, int x0, int y0, int w, int h)
{
    unsigned long long sum = 0;
    long count = 0;
    for (int y = y0; y < y0 + h && y < image.height(); ++y) {
        for (int x = x0; x < x0 + w && x < image.width(); ++x) {
            sum += qGray(image.pixel(x, y));
            ++count;
        }
    }
    return count ? static_cast<double>(sum) / count : 0.0;
}

// Mean of a single channel over a region, 0..255.
double regionChannel(const QImage& image, int x0, int y0, int w, int h, char channel)
{
    unsigned long long sum = 0;
    long count = 0;
    for (int y = y0; y < y0 + h && y < image.height(); ++y) {
        for (int x = x0; x < x0 + w && x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            switch (channel) {
            case 'r': sum += qRed(p); break;
            case 'g': sum += qGreen(p); break;
            default:  sum += qBlue(p); break;
            }
            ++count;
        }
    }
    return count ? static_cast<double>(sum) / count : 0.0;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QImage source = gradientSource(192);

    // Colour-temperature check on a neutral ramp: anime shading pushes dark
    // grey toward blue-violet and light grey toward warm.
    QImage greyRamp(192, 192, QImage::Format_RGB32);
    for (int y = 0; y < 192; ++y) {
        const int v = y * 255 / 191;
        for (int x = 0; x < 192; ++x) {
            greyRamp.setPixel(x, y, qRgb(v, v, v));
        }
    }

    TextureOptions opts;
    opts.resolution = 256;
    opts.square = true;
    opts.generateAlpha = false;

    const TextureResult realistic = TextureGenerator::preview(source, opts);
    opts.styleMode = StyleMode::Anime;
    const TextureResult anime = TextureGenerator::preview(source, opts);

    const QImage rBase = realistic.images.value(QStringLiteral("base"));
    const QImage aBase = anime.images.value(QStringLiteral("base"));
    const QImage aRough = anime.images.value(QStringLiteral("roughness"));
    const QImage rRough = realistic.images.value(QStringLiteral("roughness"));

    const int rColors = uniqueColorCount(rBase);
    const int aColors = uniqueColorCount(aBase);

    // Edge band: a horizontal strip on the hard mid edge (y in [128, 144] of
    // the 256px output). Anime must darken it with the outline.
    const double rEdge = regionGray(rBase, 0, 128, rBase.width(), 16);
    const double aEdge = regionGray(aBase, 0, 128, aBase.width(), 16);

    // Hue preservation: the top half stays red in both styles.
    double rRed = 0.0;
    double aRed = 0.0;
    for (int y = 4; y < 60; ++y) {
        for (int x = 4; x < 60; ++x) {
            rRed += qRed(rBase.pixel(x, y));
            aRed += qRed(aBase.pixel(x, y));
        }
    }
    rRed /= 56.0 * 56.0;
    aRed /= 56.0 * 56.0;
    // And the bottom half stays blue.
    double rBlue = 0.0;
    double aBlue = 0.0;
    for (int y = rBase.height() - 60; y < rBase.height() - 4; ++y) {
        for (int x = 4; x < 60; ++x) {
            rBlue += qBlue(rBase.pixel(x, y));
            aBlue += qBlue(aBase.pixel(x, y));
        }
    }
    rBlue /= 56.0 * 56.0;
    aBlue /= 56.0 * 56.0;

    // Roughness pull-down: anime mean roughness must be clearly below realistic.
    double rRoughMean = 0.0;
    double aRoughMean = 0.0;
    for (int y = 0; y < rRough.height(); ++y) {
        for (int x = 0; x < rRough.width(); ++x) {
            rRoughMean += qGray(rRough.pixel(x, y));
            aRoughMean += qGray(aRough.pixel(x, y));
        }
    }
    rRoughMean /= rRough.width() * rRough.height();
    aRoughMean /= aRough.width() * aRough.height();

    std::printf("STYLE realCols=%d animeCols=%d  -> %s\n",
                rColors, aColors,
                aColors < rColors && aColors < rColors * 3 / 4 ? "POSTERIZED" : "not posterized");
    std::printf("STYLE edge real=%.0f anime=%.0f (%s)\n",
                rEdge, aEdge,
                aEdge < rEdge - 4.0 ? "OUTLINED" : "no outline");
    std::printf("STYLE hue redTop real=%.0f anime=%.0f blueBottom real=%.0f anime=%.0f (%s)\n",
                rRed, aRed, rBlue, aBlue,
                aRed > 100 && aBlue > 100 ? "HUE_KEPT" : "hue lost");
    std::printf("STYLE roughness real=%.0f anime=%.0f (%s)\n",
                rRoughMean, aRoughMean,
                aRoughMean < rRoughMean * 0.75 ? "SMOOTHED" : "not smoothed");

    // Colour temperature on the neutral ramp.
    TextureOptions tempOpts = opts;
    tempOpts.styleMode = StyleMode::Anime;
    const QImage tempBase = TextureGenerator::preview(greyRamp, tempOpts).images.value(QStringLiteral("base"));
    // dark band: top ~20% of the ramp; light band: bottom ~20%.
    const double darkR = regionChannel(tempBase, 80, 8, 32, 32, 'r');
    const double darkB = regionChannel(tempBase, 80, 8, 32, 32, 'b');
    const double lightR = regionChannel(tempBase, 80, tempBase.height() - 40, 32, 32, 'r');
    const double lightB = regionChannel(tempBase, 80, tempBase.height() - 40, 32, 32, 'b');
    const bool coolDark = darkB > darkR + 1.0;
    const bool warmLight = lightR > lightB + 1.0;
    std::printf("STYLE temp dark R=%.0f B=%.0f light R=%.0f B=%.0f (%s)\n",
                darkR, darkB, lightR, lightB,
                coolDark && warmLight ? "COOL_SHADOW_WARM_HILIGHT" : "no temp split");
    std::fflush(stdout);

    return 0;
}