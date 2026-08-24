// Verifies the material-ball interaction contract:
//   1. default pose is a straight front view (yaw 0, pitch 0);
//   2. grab/trackball convention: the surface follows the cursor — drag
//      UP tips the front up and swings the BOTTOM into view, drag DOWN swings
//      the TOP back (the opposite of Blender's view-orbit, which this preview
//      deliberately does not follow);
//   3. the idle turntable freezes while the button is held and resumes after
//      release (no hole, full 360 degree orbit along the way).
// Uses a red-top / blue-bottom texture as the up/down discriminator.
#include "MaterialPreviewWidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QMap>
#include <QMouseEvent>
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

QImage splitBase(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.setPixel(x, y, y < size / 2 ? qRgb(230, 40, 40) : qRgb(40, 60, 230));
        }
    }
    return image;
}

void pressMove(QWidget* widget, int dx, int dy)
{
    const QPoint start(256, 256);
    QMouseEvent press(QEvent::MouseButtonPress, start, widget->mapToGlobal(start),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);

    const QPoint end(256 + dx, 256 + dy);
    QMouseEvent move(QEvent::MouseMove, end, widget->mapToGlobal(end),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &move);
}

void releaseBtn(QWidget* widget)
{
    const QPoint p(256, 256);
    QMouseEvent release(QEvent::MouseButtonRelease, p, widget->mapToGlobal(p),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

// Returns true when the upper third of the frame is red-dominant, i.e. the
// texture's top (red half) is on the ball's upper area on screen.
bool topShown(const QImage& image)
{
    long red = 0;
    long blue = 0;
    long count = 0;
    for (int y = 0; y < image.height() / 3; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            red += qRed(p);
            blue += qBlue(p);
            ++count;
        }
    }
    const double r = count ? static_cast<double>(red) / count : 0.0;
    const double b = count ? static_cast<double>(blue) / count : 0.0;
    std::printf("  R=%.0f B=%.0f -> %s\n", r, b, r > b ? "TOP" : "BOTTOM");
    std::fflush(stdout);
    return r > b;
}

double frameMean(const QImage& image)
{
    long sum = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            sum += qGray(image.pixel(x, y));
        }
    }
    const long count = static_cast<long>(image.height()) / 4 * (static_cast<long>(image.width()) / 4);
    return count ? static_cast<double>(sum) / count : 0.0;
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
    QMap<QString, QImage> maps;
    maps.insert(QStringLiteral("base"), splitBase(size));
    maps.insert(QStringLiteral("bump"), grayImage(size, 128));
    maps.insert(QStringLiteral("normal"), [size]() {
        QImage n(size, size, QImage::Format_RGB32);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                n.setPixel(x, y, qRgb(128, 128, 255));
        return n;
    }());
    maps.insert(QStringLiteral("roughness"), grayImage(size, 128));
    maps.insert(QStringLiteral("metal"), grayImage(size, 0));
    maps.insert(QStringLiteral("ao"), grayImage(size, 255));
    maps.insert(QStringLiteral("alpha"), grayImage(size, 255));

    MaterialPreviewWidget widget;
    widget.resize(512, 512);
    widget.setMaps(maps);
    widget.show();

    QTimer::singleShot(1200, &app, [&]() {
        // 1. Default pose straight-on (yaw 0, pitch 0): top of the ball should
        //    be red in the upper third of the frame.
        pressMove(&widget, 0, 0); // hold: freeze turntable
        QApplication::processEvents();
        const bool defTop = topShown(widget.grabFramebuffer());
        std::printf("PITCH_DEFAULT %s (expect TOP = red)\n", defTop ? "PASS" : "FAIL");

        // 2. Grab convention: surface follows the cursor. Drag UP -> the front
        //    tips up -> the BOTTOM swings into view (blue).
        pressMove(&widget, 0, -240);
        QApplication::processEvents();
        const bool upTop = topShown(widget.grabFramebuffer());
        std::printf("PITCH_DRAG_UP %s (expect BOTTOM = blue, not top)\n", upTop ? "FAIL still top" : "PASS");
        releaseBtn(&widget);

        //    Drag DOWN (net) -> the TOP swings back into view (red).
        pressMove(&widget, 0, 480);
        QApplication::processEvents();
        const bool downTop = topShown(widget.grabFramebuffer());
        std::printf("PITCH_DRAG_DOWN %s (expect TOP = red)\n", downTop ? "PASS" : "FAIL");
        releaseBtn(&widget);

        // 3. Full orbit with the button held each step: no clamp, no hole.
        double minMean = 1e9;
        for (int i = 0; i < 40; ++i) {
            pressMove(&widget, 0, 42); // pitch += 14.7 deg
            QApplication::processEvents();
            const double mean = frameMean(widget.grabFramebuffer());
            if (mean < minMean) {
                minMean = mean;
            }
            releaseBtn(&widget);
        }
        std::printf("PITCH_FULLORBIT minMean=%.1f (dark hole if < 30)\n", minMean);

        // 4. Turntable resumes after release: two frames 500 ms apart differ.
        const QImage a = widget.grabFramebuffer();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 500) {
            QApplication::processEvents();
        }
        const QImage b = widget.grabFramebuffer();
        long diff = 0;
        for (int y = 0; y < 512; y += 8) {
            for (int x = 0; x < 512; x += 8) {
                diff += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
            }
        }
        std::printf("SPIN_RESUME frameDiff=%ld (>200 = resumed spinning)\n", diff);
        std::fflush(stdout);

        app.quit();
    });

    return app.exec();
}
