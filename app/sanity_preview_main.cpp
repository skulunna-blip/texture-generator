// Headless-ish sanity check for MaterialPreviewWidget: render a synthetic
// material ball, grab the framebuffer and report the mean brightness so we can
// tell black (broken GL path) from a lit ball without needing real maps.
#include "MaterialPreviewWidget.h"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QMap>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdio>

namespace
{
QImage grayImage(int size, int value)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.setPixel(x, y, qRgb(value, value, value));
        }
    }
    return image;
}

QImage normalImage(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.setPixel(x, y, qRgb(128, 128, 255)); // flat identity normal
        }
    }
    return image;
}
}

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);

    constexpr int size = 256;
    QImage base(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            base.setPixel(x, y, qRgb(70 + (x * 160) / size, 90, 50 + (y * 130) / size));
        }
    }

    QMap<QString, QImage> maps;
    maps.insert(QStringLiteral("base"), base);
    maps.insert(QStringLiteral("bump"), grayImage(size, 128));
    maps.insert(QStringLiteral("normal"), normalImage(size));
    maps.insert(QStringLiteral("roughness"), grayImage(size, 128));
    maps.insert(QStringLiteral("metal"), grayImage(size, 0));
    maps.insert(QStringLiteral("ao"), grayImage(size, 255));
    maps.insert(QStringLiteral("alpha"), grayImage(size, 255));

    MaterialPreviewWidget widget;
    widget.resize(512, 512);
    widget.setMaps(maps);
    widget.show();

    auto meanGray = [&]() -> double {
        const QImage shot = widget.grabFramebuffer();
        double sum = 0.0;
        long count = 0;
        for (int y = 0; y < shot.height(); y += 4) {
            for (int x = 0; x < shot.width(); x += 4) {
                sum += qGray(shot.pixel(x, y));
                ++count;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    int failures = 0;

    QTimer::singleShot(1800, &app, [&]() {
        const double full = meanGray();
        std::printf("SANITY_MEAN_GRAY %.2f\n", full);
        std::fflush(stdout);
        widget.setPreviewMode(MaterialPreviewWidget::PreviewMode::NormalOnly);
    });
    QTimer::singleShot(2600, &app, [&]() {
        const double normalOnly = meanGray();
        std::printf("SANITY_NORMAL_MEAN %.2f\n", normalOnly);
        std::fflush(stdout);
        // "Normal Only" is the normal map's own pixels. The flat identity
        // normal fed in here is (128,128,255), so the ball has to come back
        // blue-dominant; a lit grey surface is exactly what this mode must not
        // be any more.
        const QImage shot = widget.grabFramebuffer();
        const QRgb centre = shot.pixel(shot.width() / 2, shot.height() / 2);
        const bool blueDominant = qBlue(centre) > qRed(centre) + 40
            && qBlue(centre) > qGreen(centre) + 40;
        std::printf("SANITY_NORMAL_VIEW %s r=%d g=%d b=%d\n", blueDominant ? "PASS" : "FAIL",
                    qRed(centre), qGreen(centre), qBlue(centre));
        std::fflush(stdout);
        if (!blueDominant) {
            ++failures;
        }
        widget.setPreviewMode(MaterialPreviewWidget::PreviewMode::BumpOnly);
    });
    QTimer::singleShot(3400, &app, [&]() {
        const double bumpOnly = meanGray();
        std::printf("SANITY_BUMP_MEAN %.2f\n", bumpOnly);
        std::fflush(stdout);
        app.quit();
    });

    const int code = app.exec();
    std::printf("SANITY %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? code : 1;
}
