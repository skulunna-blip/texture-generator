// Rotation sweep for MaterialPreviewWidget: simulate an interaction (stops the
// idle turntable), then sweep the ball through a full rotation and pitch
// extremes, grabbing a frame at each angle. Reports center/frame brightness so
// we can catch a "backface-culling"-style hole (some angle where the closed
// sphere suddenly shows the background or the dark far hemisphere).
#include "MaterialPreviewWidget.h"

#include <QApplication>
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

QImage normalImage(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.setPixel(x, y, qRgb(128, 128, 255));
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

double regionMean(const QImage& image, double centerFraction)
{
    const int cx = image.width() / 2;
    const int cy = image.height() / 2;
    const int r = static_cast<int>(std::min(image.width(), image.height()) * centerFraction / 2.0);
    long sum = 0;
    long count = 0;
    for (int y = cy - r; y < cy + r; ++y) {
        for (int x = cx - r; x < cx + r; ++x) {
            sum += qGray(image.pixel(x, y));
            ++count;
        }
    }
    return count > 0 ? static_cast<double>(sum) / count : 0.0;
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
            const int checker = ((x / 32 + y / 32) % 2) ? 40 : 210;
            base.setPixel(x, y, qRgb(checker, 90, 120));
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

    QTimer::singleShot(1200, &app, [&]() {
        double minFrame = 999.0;
        double minCenter = 999.0;
        int worstIndex = -1;
        std::printf("ROTATE_SWEEP\n");
        std::fflush(stdout);

        // Full rotation: 12 x 36 deg around Y. Each step holds the button so the
        // idle turntable stays frozen (it resumes after release), making every
        // shot exactly where the drag left it.
        for (int step = 0; step < 12; ++step) {
            pressMove(&widget, 80, 0); // +36 deg yaw
            QApplication::processEvents();
            const QImage shot = widget.grabFramebuffer();
            const double frame = regionMean(shot, 1.0);
            const double center = regionMean(shot, 0.5);
            std::printf("yaw_step=%2d frame=%.1f center=%.1f\n", step, frame, center);
            std::fflush(stdout);
            if (frame < minFrame) {
                minFrame = frame;
                minCenter = center;
                worstIndex = step;
            }
            shot.save(QStringLiteral("rot_%1.png").arg(step));
            releaseBtn(&widget);
            QApplication::processEvents();
        }

        // Pitch extremes from the current yaw: +60 (see top) then -60 (bottom).
        pressMove(&widget, 0, 0);
        QApplication::processEvents();
        const QImage top = widget.grabFramebuffer();
        top.save(QStringLiteral("rot_top.png"));
        std::printf("pitch=0  frame=%.1f center=%.1f\n", regionMean(top, 1.0), regionMean(top, 0.5));
        pressMove(&widget, 0, -171); // pitch += 60
        QApplication::processEvents();
        const QImage topShot = widget.grabFramebuffer();
        topShot.save(QStringLiteral("rot_p60.png"));
        std::printf("pitch=+60 frame=%.1f center=%.1f\n", regionMean(topShot, 1.0), regionMean(topShot, 0.5));
        releaseBtn(&widget);
        pressMove(&widget, 0, 342); // pitch back to 0 then -60
        QApplication::processEvents();
        const QImage bottomShot = widget.grabFramebuffer();
        bottomShot.save(QStringLiteral("rot_m60.png"));
        std::printf("pitch=-60 frame=%.1f center=%.1f\n", regionMean(bottomShot, 1.0), regionMean(bottomShot, 0.5));
        releaseBtn(&widget);

        std::printf("RESULT worst_yaw_step=%d minFrame=%.1f minCenter=%.1f\n", worstIndex, minFrame, minCenter);
        std::fflush(stdout);
        app.quit();
    });

    return app.exec();
}
