// Headless check for the monocular depth backend.
//
// The model and the ONNX Runtime DLL are optional at run time, so a missing
// dependency prints SKIP and returns 0 rather than pretending to pass - the
// same convention tg_upscale_test uses for its absent binary.
#include "DepthEstimator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QRadialGradient>
#include <QLinearGradient>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
        ++g_failures;
    }
}

// A bright disc over a corner-to-corner gradient. Verified against the Python
// reference: the model reads the disc as NEARER than the corners, so this is a
// real polarity assertion rather than a guess about what "should" happen.
QImage makeScene(int size)
{
    QImage image(size, size, QImage::Format_ARGB32);
    QPainter painter(&image);
    QLinearGradient background(0, 0, size, size);
    background.setColorAt(0.0, QColor(40, 40, 40));
    background.setColorAt(1.0, QColor(190, 190, 190));
    painter.fillRect(image.rect(), background);
    QRadialGradient disc(QPointF(size * 0.5, size * 0.5), size * 0.28);
    disc.setColorAt(0.0, QColor(235, 225, 210));
    disc.setColorAt(1.0, QColor(235, 225, 210));
    painter.setBrush(disc);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(size * 0.5, size * 0.5), size * 0.28, size * 0.28);
    painter.end();
    return image;
}

double regionMean(const QImage& depth, double x0, double y0, double x1, double y1)
{
    const int w = depth.width();
    const int h = depth.height();
    const int ax = std::max(0, static_cast<int>(w * x0));
    const int bx = std::min(w, static_cast<int>(w * x1));
    const int ay = std::max(0, static_cast<int>(h * y0));
    const int by = std::min(h, static_cast<int>(h * y1));
    long sum = 0;
    long count = 0;
    for (int y = ay; y < by; ++y) {
        const uchar* line = depth.constScanLine(y);
        for (int x = ax; x < bx; ++x) {
            sum += line[x];
            ++count;
        }
    }
    return count > 0 ? static_cast<double>(sum) / count : 0.0;
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
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("tg_depth_check\n");
    std::printf("  library: %s\n",
                DepthEstimator::findLibrary().isEmpty()
                    ? "(not found)"
                    : qPrintable(DepthEstimator::findLibrary()));
    std::printf("  model:   %s\n",
                DepthEstimator::findModel().isEmpty()
                    ? "(not found)"
                    : qPrintable(DepthEstimator::findModel()));

    if (!DepthEstimator::isAvailable()) {
        std::printf("SKIP  depth backend not installed "
                    "(set TG_ONNXRUNTIME_DLL / TG_DEPTH_MODEL to point at them)\n");
        return 0;
    }

    const QImage scene = makeScene(512);

    std::printf("\n[1] shape and range\n");
    QElapsedTimer timer;
    timer.start();
    const QImage depth = DepthEstimator::estimate(scene, 518);
    const qint64 firstMs = timer.elapsed();
    check(!depth.isNull(), "estimate() returns an image");
    check(depth.size() == scene.size(), "depth matches the source size");
    check(depth.format() == QImage::Format_Grayscale8, "depth is Grayscale8");

    uchar lo = 255;
    uchar hi = 0;
    for (int y = 0; y < depth.height(); ++y) {
        const uchar* line = depth.constScanLine(y);
        for (int x = 0; x < depth.width(); ++x) {
            lo = std::min(lo, line[x]);
            hi = std::max(hi, line[x]);
        }
    }
    check(hi - lo > 40, "depth carries real structure (range > 40 levels)");
    std::printf("      range %d..%d, first run %lld ms\n", lo, hi, static_cast<long long>(firstMs));

    std::printf("\n[2] polarity: the disc must read as nearer than the corners\n");
    const double centre = regionMean(depth, 0.42, 0.42, 0.58, 0.58);
    const double corner = (regionMean(depth, 0.0, 0.0, 0.12, 0.12)
                           + regionMean(depth, 0.88, 0.88, 1.0, 1.0)) / 2.0;
    check(centre > corner, "disc is brighter (nearer) than the background");
    std::printf("      centre %.1f vs corner %.1f\n", centre, corner);

    std::printf("\n[3] cache: the second call must be identical and near-free\n");
    timer.restart();
    const QImage again = DepthEstimator::estimate(scene, 518);
    const qint64 cachedMs = timer.elapsed();
    check(bytesEqual(depth, again), "cached result is byte-identical");
    check(cachedMs * 4 < firstMs, "cached call is at least 4x faster");
    std::printf("      %lld ms vs %lld ms\n",
                static_cast<long long>(cachedMs),
                static_cast<long long>(firstMs));

    std::printf("\n[4] a different image must not reuse the cache\n");
    QImage other = makeScene(512);
    QPainter painter(&other);
    painter.fillRect(QRect(0, 0, 256, 512), QColor(20, 60, 140));
    painter.end();
    const QImage otherDepth = DepthEstimator::estimate(other, 518);
    check(!otherDepth.isNull(), "second image produces a depth map");
    check(!bytesEqual(depth, otherDepth), "second image differs from the first");

    std::printf("\n[5] preview resolution must be faster than the final one\n");
    QImage preview = makeScene(512);
    preview.setPixel(0, 0, qRgb(1, 2, 3)); // bust the cache without changing meaning
    timer.restart();
    const QImage fast = DepthEstimator::estimate(preview, 322);
    const qint64 fastMs = timer.elapsed();
    check(!fast.isNull() && fast.size() == preview.size(), "322 input still fills the frame");
    std::printf("      518: %lld ms   322: %lld ms\n",
                static_cast<long long>(firstMs),
                static_cast<long long>(fastMs));
    check(fastMs < firstMs, "322 is faster than 518");

    std::printf("\ntg_depth_check: %s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
