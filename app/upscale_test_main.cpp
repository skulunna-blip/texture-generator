// AI super-resolution link check. When realesrgan-ncnn-vulkan is bundled
// (tools/ under the project, beside the exe, or via TG_REALESRGAN_BIN) this
// runs a real 4x pass over synthetic maps and asserts every output grew to the
// expected size. When it is missing it reports the expected-not-found state so
// the test stays green until the user drops the binary in.
#include "UpscaleEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>

#include <cstdio>

namespace
{
void fill(QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixel(x, y, qRgb((x * 2) % 256, (y * 2) % 256, 128));
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (!UpscaleEngine::isAvailable()) {
        std::printf("UPSCALE REALESRGAN_NOT_FOUND (expected until the binary is placed in tools/)\n");
        return 0;
    }

    const QString binary = UpscaleEngine::findBinary();
    std::printf("UPSCALE binary=%s\n", qPrintable(binary));

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::printf("UPSCALE FAIL no temp dir\n");
        return 1;
    }
    const QString root = temp.path();

    const QStringList keys = {
        QStringLiteral("base"),  QStringLiteral("bump"),   QStringLiteral("normal"),
        QStringLiteral("roughness"), QStringLiteral("metal"), QStringLiteral("ao"),
        QStringLiteral("alpha"),
    };
    QMap<QString, QString> sources;
    for (const QString& key : keys) {
        const QString path = QDir(root).filePath(key + QStringLiteral(".png"));
        QImage image(128, 128, QImage::Format_RGB32);
        fill(image);
        image.save(path, "PNG");
        sources.insert(key, path);
    }

    // Realistic style -> realesrgan-x4plus; the model dir is implied by the
    // binary location, so a missing models/ folder surfaces as a run failure.
    const UpscaleEngine::Result result = UpscaleEngine::upscale(sources, root, 4, StyleMode::Realistic);
    if (!result.ok) {
        std::printf("UPSCALE FAIL %s\n", qPrintable(result.error));
        return 1;
    }

    bool allGood = true;
    for (const QString& key : keys) {
        const QImage image = result.images.value(key);
        const bool expected = image.width() == 512 && image.height() == 512;
        std::printf("UPSCALE %-8s %dx%d -> %dx%d %s\n",
                    qPrintable(key), 128, 128,
                    image.width(), image.height(),
                    expected ? "OK" : "BAD");
        if (!expected) {
            allGood = false;
        }
    }

    // 2x is the mode that used to be broken: the CLI's own -s 2 writes the
    // top-left quarter of the 4x result. A size check cannot see that, so this
    // one checks content - four coloured quadrants have to survive the round
    // trip, each landing in its own corner.
    {
        const int side = 64;
        QImage quad(side, side, QImage::Format_RGB32);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const bool right = x >= side / 2;
                const bool bottom = y >= side / 2;
                const QRgb colour = bottom ? (right ? qRgb(255, 255, 0) : qRgb(0, 0, 255))
                                           : (right ? qRgb(0, 255, 0) : qRgb(255, 0, 0));
                quad.setPixel(x, y, colour);
            }
        }
        const QString quadPath = QDir(root).filePath(QStringLiteral("quad.png"));
        quad.save(quadPath, "PNG");

        QMap<QString, QString> one;
        one.insert(QStringLiteral("base"), quadPath);
        const UpscaleEngine::Result twice = UpscaleEngine::upscale(one, root, 2, StyleMode::Realistic);
        const QImage out = twice.images.value(QStringLiteral("base"));
        const int w = out.width();
        const int h = out.height();
        bool quadOk = w == side * 2 && h == side * 2;
        if (quadOk) {
            auto near = [&](int x, int y, int r, int g, int b) {
                const QRgb p = out.pixel(x, y);
                return std::abs(qRed(p) - r) < 60 && std::abs(qGreen(p) - g) < 60
                    && std::abs(qBlue(p) - b) < 60;
            };
            quadOk = near(w / 4, h / 4, 255, 0, 0) && near(3 * w / 4, h / 4, 0, 255, 0)
                  && near(w / 4, 3 * h / 4, 0, 0, 255) && near(3 * w / 4, 3 * h / 4, 255, 255, 0);
        }
        std::printf("UPSCALE 2x     %dx%d -> %dx%d %s\n", side, side, w, h,
                    quadOk ? "OK (all four quadrants kept)" : "BAD (image was cropped)");
        if (!quadOk) {
            allGood = false;
        }
    }

    // The source path: one image in, one bigger image out, with progress along
    // the way. This is what the app actually calls, and the progress callback is
    // what the UI's bar is fed from, so both are checked here rather than only
    // the batch API.
    {
        const int side = 96;
        QImage source(side, side, QImage::Format_RGB32);
        fill(source);
        const QString inPath = QDir(root).filePath(QStringLiteral("source_in.png"));
        const QString outPath = QDir(root).filePath(QStringLiteral("source_out.png"));
        source.save(inPath, "PNG");

        QList<int> seen;
        QString error;
        const bool ok = UpscaleEngine::upscaleImage(
            inPath, outPath, 4, StyleMode::Realistic,
            [&](int percent, const QString&) { seen.append(percent); }, &error);

        QImage result(outPath);
        bool monotonic = true;
        for (int i = 1; i < seen.size(); ++i) {
            if (seen.at(i) < seen.at(i - 1)) {
                monotonic = false;
            }
        }
        const bool sizeOk = result.width() == side * 4 && result.height() == side * 4;
        const bool progressOk = !seen.isEmpty() && seen.first() == 0 && seen.last() == 100
                             && monotonic && seen.size() >= 2;
        std::printf("UPSCALE source %dx%d -> %dx%d %s  progress reports=%lld monotonic=%s %s\n",
                    side, side, result.width(), result.height(), (ok && sizeOk) ? "OK" : "BAD",
                    static_cast<long long>(seen.size()), monotonic ? "yes" : "NO",
                    progressOk ? "OK" : "BAD");
        if (!ok || !sizeOk || !progressOk) {
            allGood = false;
        }
    }

    std::printf("UPSCALE %s\n", allGood ? "PASS 4x all maps + 2x content"
                                       : "FAIL");
    return allGood ? 0 : 1;
}
