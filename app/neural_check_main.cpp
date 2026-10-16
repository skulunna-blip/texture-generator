// End-to-end check for the de-lighting + neural-depth wiring in
// TextureGenerator::buildResult.
//
// Two things matter here and neither is covered by the unit tests:
//   1. with the new options off, the pipeline output must be byte-identical to
//      what the app produced before the feature existed - no silent regression;
//   2. with them on, the AI backend must actually run instead of throwing.
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QElapsedTimer>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
        ++g_failures;
    }
}

bool bytesEqual(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.format() != b.format()) {
        return false;
    }
    const int bytes = a.width() * a.depth() / 8;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), bytes) != 0) {
            return false;
        }
    }
    return true;
}

// A photo-like scene: a lit surface with a strong directional falloff plus a
// specular hotspot, so there is real shading to remove.
QImage makeLitPhoto(int size)
{
    QImage image(size, size, QImage::Format_ARGB32);
    QPainter painter(&image);
    QLinearGradient light(0, 0, size, size);
    light.setColorAt(0.0, QColor(252, 246, 232));
    light.setColorAt(0.55, QColor(168, 152, 132));
    light.setColorAt(1.0, QColor(58, 52, 46));
    painter.fillRect(image.rect(), light);
    QRadialGradient hotspot(QPointF(size * 0.3, size * 0.28), size * 0.2);
    hotspot.setColorAt(0.0, QColor(255, 255, 255));
    hotspot.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setBrush(hotspot);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(size * 0.3, size * 0.28), size * 0.2, size * 0.2);
    painter.end();
    return image;
}

double meanLuma(const QImage& image)
{
    double sum = 0;
    long count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            sum += 0.2126 * qRed(p) + 0.7152 * qGreen(p) + 0.0722 * qBlue(p);
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

double maxLuma(const QImage& image)
{
    double best = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            best = std::max(best, 0.2126 * qRed(p) + 0.7152 * qGreen(p) + 0.0722 * qBlue(p));
        }
    }
    return best;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("tg_neural_check\n");

    const QImage photo = makeLitPhoto(512);

    TextureOptions base;
    base.resolution = 512;
    base.square = true;
    base.backendMode = TextureBackendMode::EnhancedDetail;

    std::printf("\n[1] defaults must reproduce the pre-feature pipeline exactly\n");
    TextureOptions legacy = base;
    const TextureResult legacyResult = TextureGenerator::preview(photo, legacy);
    TextureOptions same = base;
    same.removeLighting = false;
    same.delightStrength = 0.8;
    same.aiPreview = false;
    const TextureResult sameResult = TextureGenerator::preview(photo, same);
    check(!legacyResult.images.isEmpty(), "classic preview produces maps");
    check(legacyResult.images.size() == sameResult.images.size(), "same map set");
    for (auto it = legacyResult.images.constBegin(); it != legacyResult.images.constEnd(); ++it) {
        check(bytesEqual(it.value(), sameResult.images.value(it.key())),
              qPrintable(QStringLiteral("unchanged: %1").arg(it.key())));
    }

    std::printf("\n[2] de-lighting flattens the lighting without dimming the map\n");
    TextureOptions delit = base;
    delit.removeLighting = true;
    delit.delightStrength = 1.0;
    const TextureResult delitResult = TextureGenerator::preview(photo, delit);
    check(!delitResult.images.isEmpty(), "de-lit preview produces maps");
    const QImage before = legacyResult.images.value(QStringLiteral("base"));
    const QImage after = delitResult.images.value(QStringLiteral("base"));
    check(!bytesEqual(before, after), "base color actually changed");

    const double beforePeak = maxLuma(before);
    const double afterPeak = maxLuma(after);
    check(afterPeak < beforePeak, "the specular hotspot was pulled down");
    std::printf("      peak luma %.1f -> %.1f, mean %.1f -> %.1f\n",
                beforePeak, afterPeak, meanLuma(before), meanLuma(after));
    check(meanLuma(after) > meanLuma(before) * 0.6,
          "the map did not simply go dark (mean kept above 60%)");

    std::printf("\n[3] alpha must survive de-lighting byte for byte\n");
    QImage transparent = photo.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < transparent.height(); ++y) {
        for (int x = 0; x < transparent.width(); ++x) {
            if (x < transparent.width() / 3) {
                transparent.setPixel(x, y, qRgba(qRed(transparent.pixel(x, y)),
                                                 qGreen(transparent.pixel(x, y)),
                                                 qBlue(transparent.pixel(x, y)),
                                                 (x * 255) / (transparent.width() / 3)));
            }
        }
    }
    const TextureResult alphaPlain = TextureGenerator::preview(transparent, base);
    const TextureResult alphaDelit = TextureGenerator::preview(transparent, delit);
    check(!alphaPlain.images.value(QStringLiteral("alpha")).isNull(), "alpha map produced");
    check(bytesEqual(alphaPlain.images.value(QStringLiteral("alpha")),
                     alphaDelit.images.value(QStringLiteral("alpha"))),
          "alpha map is identical with de-lighting on");

    std::printf("\n[4] AI backend availability\n");
    const bool available = TextureGenerator::isBackendAvailable(TextureBackendMode::AiBridge);
    std::printf("      DepthEstimator available: %s\n", available ? "yes" : "no");
    if (!available) {
        std::printf("SKIP  neural pipeline checks (model or onnxruntime.dll not installed)\n");
        std::printf("\ntg_neural_check: %s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
        return g_failures == 0 ? 0 : 1;
    }

    std::printf("\n[5] the AI backend must run rather than throw\n");
    TextureOptions neural = base;
    neural.backendMode = TextureBackendMode::AiBridge;
    bool threw = false;
    TextureResult neuralResult;
    try {
        neuralResult = TextureGenerator::preview(photo, neural);
    } catch (const std::exception& exception) {
        threw = true;
        std::printf("      threw: %s\n", exception.what());
    }
    check(!threw, "AiBridge preview no longer throws");
    check(!neuralResult.images.isEmpty(), "neural preview produced maps");

    std::printf("\n[6] neural height must differ from the pure-luminance height\n");
    const QImage classicHeight = legacyResult.images.value(QStringLiteral("bump"));
    const QImage neuralHeight = neuralResult.images.value(QStringLiteral("bump"));
    check(!neuralHeight.isNull(), "neural bump map exists");
    check(!bytesEqual(classicHeight, neuralHeight), "depth relief changed the height map");

    std::printf("\n[7] AI backend + de-lighting together\n");
    TextureOptions both = neural;
    both.removeLighting = true;
    both.delightStrength = 1.0;
    bool bothThrew = false;
    TextureResult bothResult;
    try {
        bothResult = TextureGenerator::preview(photo, both);
    } catch (const std::exception& exception) {
        bothThrew = true;
        std::printf("      threw: %s\n", exception.what());
    }
    check(!bothThrew, "combined path runs");
    check(!bothResult.images.isEmpty(), "combined path produced maps");
    check(!bytesEqual(bothResult.images.value(QStringLiteral("base")),
                      neuralResult.images.value(QStringLiteral("base"))),
          "de-lighting changed the neural base color too");

    std::printf("\n[8] dragging a slider must not re-run inference\n");
    // This is the whole reason the depth map is cached: the model only depends
    // on the source, so a slider change has to be a pure CPU pass over an
    // already-computed depth map.
    TextureOptions draggable = neural;
    draggable.aiPreview = true;
    QElapsedTimer timer;
    timer.start();
    const TextureResult first = TextureGenerator::preview(photo, draggable);
    const qint64 firstMs = timer.elapsed();
    draggable.bumpStrength = 1.7;
    draggable.normalStrength = 1.4;
    timer.restart();
    const TextureResult second = TextureGenerator::preview(photo, draggable);
    const qint64 secondMs = timer.elapsed();
    check(!first.images.isEmpty() && !second.images.isEmpty(), "both previews produced maps");
    check(!bytesEqual(first.images.value(QStringLiteral("normal")),
                      second.images.value(QStringLiteral("normal"))),
          "the strength sliders did change the normal map");
    check(secondMs * 3 < firstMs, "the second pass is at least 3x faster (depth was cached)");
    std::printf("      with inference %lld ms, cached %lld ms\n",
                static_cast<long long>(firstMs), static_cast<long long>(secondMs));

    std::printf("\ntg_neural_check: %s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
