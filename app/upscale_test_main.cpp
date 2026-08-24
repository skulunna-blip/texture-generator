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

    std::printf("UPSCALE %s\n", allGood ? "PASS 128 -> 512 all maps" : "FAIL size mismatch");
    return allGood ? 0 : 1;
}
