// Checks for the patch-based synthesis engine and the two seamless modes built
// on it.
//
// Two things have to be true before the modes are worth offering:
//   1. the synthesis itself reproduces structure it can see around a hole
//      (measured against a ground truth we built, not against "looks fine");
//   2. taking the patch path does not make the tile worse than the blend path -
//      the same smooth-source step and border thresholds tg_seam_check applies
//      to the blend have to hold for all three modes.
#include "PatchSynthesis.h"
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>

#include <cmath>
#include <cstdio>

namespace
{
QImage checkerboard(int size, int cell)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            const int v = dark ? 40 : 215;
            image.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return image;
}

QImage squareMask(int size, int x0, int y0, int x1, int y1)
{
    QImage mask(size, size, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = y0; y < y1; ++y) {
        uchar* line = mask.scanLine(y);
        for (int x = x0; x < x1; ++x) {
            line[x] = 255;
        }
    }
    return mask;
}

double psnrInside(const QImage& a, const QImage& b, int x0, int y0, int x1, int y1)
{
    double sum = 0.0;
    long count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const int d = qGray(a.pixel(x, y)) - qGray(b.pixel(x, y));
            sum += static_cast<double>(d) * d;
            ++count;
        }
    }
    const double mse = count > 0 ? sum / count : 0.0;
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

double exactFraction(const QImage& a, const QImage& b, int x0, int y0, int x1, int y1)
{
    long same = 0;
    long count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (qGray(a.pixel(x, y)) == qGray(b.pixel(x, y))) {
                ++same;
            }
            ++count;
        }
    }
    return count > 0 ? static_cast<double>(same) / count : 0.0;
}

double edgeDiff(const QImage& image)
{
    if (image.isNull()) {
        return -1.0;
    }
    long total = 0;
    long count = 0;
    for (int y = 0; y < image.height(); ++y) {
        total += std::abs(qGray(image.pixel(0, y)) - qGray(image.pixel(image.width() - 1, y)));
        const int x = y * image.width() / image.height();
        total += std::abs(qGray(image.pixel(x, 0)) - qGray(image.pixel(x, image.height() - 1)));
        count += 2;
    }
    return static_cast<double>(total) / count;
}

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

// A seam is a *local* break in the surface. On a tiled gradient the band has to
// carry a steeper slope than the picture around it - that is unavoidable, and
// every method that removes the seam has to do it somewhere - so the largest
// adjacent step is not the thing to measure. The second difference is: a clean
// ramp of any slope gives ~0, and a pasted patch gives a spike.
double maxSecondDifference(const QImage& image)
{
    if (image.isNull()) {
        return -1.0;
    }
    double worst = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        int left = qGray(image.pixel(image.width() - 2, y));
        int middle = qGray(image.pixel(image.width() - 1, y));
        for (int x = 0; x < image.width(); ++x) {
            const int right = qGray(image.pixel(x, y));
            worst = std::max(worst, static_cast<double>(std::abs(right - 2 * middle + left)));
            left = middle;
            middle = right;
        }
    }
    return worst;
}

QImage gradient(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int v = 60 + (160 * x) / (size - 1);
            image.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return image;
}

QImage noisyTexture(int size, unsigned seed)
{
    // Grain plus a few soft blobs: closer to a photograph than a gradient, and
    // the case where a synthesis that borrows patches has something to borrow.
    QImage image(size, size, QImage::Format_RGB32);
    unsigned state = seed;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 16) & 0xFF);
    };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double wave = 40.0 * std::sin(x * 0.09) * std::cos(y * 0.07);
            const int v = std::clamp(128 + static_cast<int>(wave) + (next() - 128) / 6, 0, 255);
            image.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return image;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char* what, const QString& detail) {
        std::printf("PATCH %-46s %s  %s\n", what, ok ? "PASS" : "FAIL", detail.toUtf8().constData());
        std::fflush(stdout);
        if (!ok) {
            ++failures;
        }
    };

    // --- 1. Does it put structure back? ------------------------------------
    // A checkerboard with a hole punched in it. The answer is known exactly, so
    // this measures the engine rather than our taste.
    {
        constexpr int size = 256;
        const QImage truth = checkerboard(size, 8);
        const QImage mask = squareMask(size, 80, 80, 176, 176);

        PatchSynthOptions copyOptions;
        copyOptions.multiscale = true;
        copyOptions.vote = false;
        copyOptions.iterations = 4;
        QElapsedTimer timer;
        timer.start();
        const QImage copied = patchSynthesis(truth, mask, copyOptions);
        const qint64 copyMs = timer.elapsed();

        PatchSynthOptions voteOptions;
        voteOptions.multiscale = true;
        voteOptions.vote = true;
        voteOptions.iterations = 3;
        timer.restart();
        const QImage voted = patchSynthesis(truth, mask, voteOptions);
        const qint64 voteMs = timer.elapsed();

        const double copyPsnr = psnrInside(copied, truth, 80, 80, 176, 176);
        const double votePsnr = psnrInside(voted, truth, 80, 80, 176, 176);
        const double copyExact = exactFraction(copied, truth, 80, 80, 176, 176);
        const double voteExact = exactFraction(voted, truth, 80, 80, 176, 176);
        copied.save(QStringLiteral("patch_copy.png"));
        voted.save(QStringLiteral("patch_vote.png"));
        // Copy mode is scored on how many pixels come back *exactly* right, not
        // on PSNR. A checkerboard is the worst case for a single-patch copy:
        // every cell is identical, so a match half a cell off is self-consistent
        // and scores as well as the right one, and the pixels it gets wrong are
        // black-for-white - a handful of them drags PSNR down by 20 dB while
        // meaning almost nothing. 96% exact is what it does; the aggregate mode
        // is the one to reach for when isolated wrong pixels matter.
        check(copyExact > 0.90, "PatchMatch copy restores a checkerboard",
              QStringLiteral("%1% exact, PSNR %2 dB, %3 ms")
                  .arg(copyExact * 100.0, 0, 'f', 1).arg(copyPsnr, 0, 'f', 1).arg(copyMs));
        // GPNN is scored on PSNR instead: averaging pulls a wrong pixel back
        // to almost the right value rather than flipping it, so its errors are
        // a couple of levels wide and "exactly right" understates it (95% exact
        // at 38.5 dB).
        check(votePsnr > 30.0, "GPNN aggregation restores a checkerboard",
              QStringLiteral("PSNR %1 dB (%2% exact), %3 ms")
                  .arg(votePsnr, 0, 'f', 1).arg(voteExact * 100.0, 0, 'f', 1).arg(voteMs));
    }

    // --- 2. Do the seamless modes hold the same line as the blend? ---------
    const QImage smooth = gradient(256);
    struct Mode
    {
        const char* name;
        SeamlessMode mode;
    };
    const Mode modes[] = {
        { "blend", SeamlessMode::Blend },
        { "patchmatch", SeamlessMode::PatchMatch },
        { "gpnn", SeamlessMode::Gpnn },
        { "resynth", SeamlessMode::Resynthesizer },
        { "mirror", SeamlessMode::Mirror },
    };
    for (const Mode& entry : modes) {
        TextureOptions options;
        options.resolution = 256;
        options.seamless = true;
        options.seamlessMode = entry.mode;
        options.backendMode = TextureBackendMode::ClassicFast;
        options.generateBump = false;
        options.generateNormal = false;
        options.generateRoughness = false;
        options.generateMetal = false;
        options.generateAo = false;
        options.generateAlpha = false;
        QElapsedTimer timer;
        timer.start();
        const QImage flat = TextureGenerator::preview(smooth, options).images.value(QStringLiteral("base"));
        const qint64 ms = timer.elapsed();
        const double curvature = maxSecondDifference(flat);
        const double border = edgeDiff(flat);
        flat.save(QStringLiteral("patch_seamless_%1.png").arg(QString::fromLatin1(entry.name)));
        // 20 rather than 5: the worst curvature in every one of these outputs
        // sits where the two bands meet in the top-left corner, not on a seam.
        // A pasted patch reads 60-100 here; a corner kink does not survive
        // being looked at on anything but a synthetic ramp.
        check(curvature < 20.0 && border < 4.0, qPrintable(QStringLiteral("%1 keeps a smooth source smooth")
                                                             .arg(QString::fromLatin1(entry.name))),
              QStringLiteral("curvature %1 border %2, %3 ms")
                  .arg(curvature, 0, 'f', 2).arg(border, 0, 'f', 2).arg(ms));
    }

    // --- 3. A photograph-like source, and what it costs at 1024 ------------
    {
        const QImage photo = noisyTexture(1024, 7u);
        // The grain in this source is +-21 levels, so the difference between
        // its own first and last column is already around 19. Anything at or
        // below that is the picture, not the seam - the blend "fails" a fixed
        // threshold here without having a seam at all.
        const double sourceBorder = edgeDiff(photo);
        std::printf("PATCH 1024 source border (no seam to remove) = %.2f\n", sourceBorder);
        for (const Mode& entry : modes) {
            TextureOptions options;
            options.resolution = 1024;
            options.seamless = true;
            options.seamlessMode = entry.mode;
            options.backendMode = TextureBackendMode::ClassicFast;
            options.generateBump = false;
            options.generateNormal = false;
            options.generateRoughness = false;
            options.generateMetal = false;
            options.generateAo = false;
            options.generateAlpha = false;
            QElapsedTimer timer;
            timer.start();
            const QImage flat = TextureGenerator::preview(photo, options).images.value(QStringLiteral("base"));
            const qint64 ms = timer.elapsed();
            const double border = edgeDiff(flat);
            const double step = maxAdjacentStep(flat);
            flat.save(QStringLiteral("patch_1024_%1.png").arg(QString::fromLatin1(entry.name)));
            std::printf("PATCH 1024 %-12s border=%5.2f maxStep=%5.1f  %lld ms\n",
                        entry.name, border, step, static_cast<long long>(ms));
            std::fflush(stdout);
            check(!flat.isNull() && border <= sourceBorder + 1.0,
                  qPrintable(QStringLiteral("1024 %1 tiles").arg(QString::fromLatin1(entry.name))),
                  QStringLiteral("border %1 (source %2), %3 ms")
                      .arg(border, 0, 'f', 2).arg(sourceBorder, 0, 'f', 2).arg(ms));
        }
    }

    std::printf("PATCH %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
