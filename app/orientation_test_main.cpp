// Orientation check for the material ball: the map must sit on the sphere the
// same way it does on Blender's default UV sphere. Blender maps u=0 to -X
// (screen-left from the preview camera) and the image top (v=1) to +Z / the
// sphere's top. A left-red/right-blue map must therefore render redder on the
// screen's left half and bluer on the right, with no mirroring.
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
QImage gray(int size, int value)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.setPixel(x, y, qRgb(value, value, value));
        }
    }
    return image;
}

// Map that alternates red/blue in quarter-columns (RBRB). The camera looks
// along -Z, so the visible front hemisphere is exactly u=0..0.5: the screen
// left edge is the u=0 meridian (-X), the front-centre meridian (facing the
// camera) is u=0.25 (+Z), and the screen right edge is u=0.5 (+X). A
// left-red/right-blue map therefore shows RED on the screen's left half and
// BLUE on its right. The colour boundary sits on the front-centre meridian so
// one camera frame sees both colours; a plain half/half map would cover the
// whole visible hemisphere with a single colour and be undetectable.
QImage splitFrontQuarters(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(size);
            const bool isRed = (fx < 0.25f) || (fx >= 0.5f && fx < 0.75f);
            image.setPixel(x, y, isRed ? qRgb(230, 40, 40) : qRgb(40, 60, 230));
        }
    }
    return image;
}

struct HalfCounts
{
    long red = 0;
    long blue = 0;
    long count = 0;
};

HalfCounts countLeft(const QImage& image)
{
    HalfCounts c;
    const int radius = image.width() / 4; // sample inside the sphere, not the corners
    const int cx = image.width() / 2;
    const int cy = image.height() / 2;
    for (int y = cy - radius; y < cy + radius; y += 2) {
        for (int x = cx - radius; x < cx; x += 2) {
            const QRgb p = image.pixel(x, y);
            c.red += qRed(p);
            c.blue += qBlue(p);
            ++c.count;
        }
    }
    return c;
}

HalfCounts countRight(const QImage& image)
{
    HalfCounts c;
    const int radius = image.width() / 4;
    const int cx = image.width() / 2;
    const int cy = image.height() / 2;
    for (int y = cy - radius; y < cy + radius; y += 2) {
        for (int x = cx + 1; x < cx + radius; x += 2) {
            const QRgb p = image.pixel(x, y);
            c.red += qRed(p);
            c.blue += qBlue(p);
            ++c.count;
        }
    }
    return c;
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
    maps.insert(QStringLiteral("base"), splitFrontQuarters(size));
    maps.insert(QStringLiteral("bump"), gray(size, 128));
    maps.insert(QStringLiteral("normal"), [size]() {
        QImage n(size, size, QImage::Format_RGB32);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                n.setPixel(x, y, qRgb(128, 128, 255));
        return n;
    }());
    maps.insert(QStringLiteral("roughness"), gray(size, 96));
    maps.insert(QStringLiteral("metal"), gray(size, 0));
    maps.insert(QStringLiteral("ao"), gray(size, 255));
    maps.insert(QStringLiteral("alpha"), gray(size, 255));

    MaterialPreviewWidget widget;
    widget.resize(512, 512);
    widget.setMaps(maps);
    widget.show();

    // Hold the left button before the first paint runs, so the turntable never
    // advances: yaw stays at 0 and the frame shows the true initial pose.
    QMouseEvent press(QEvent::MouseButtonPress, QPoint(256, 256), widget.mapToGlobal(QPoint(256, 256)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);

    QTimer::singleShot(800, &app, [&]() {
        const QImage shot = widget.grabFramebuffer();
        shot.save(QStringLiteral("orient.png"));
        const HalfCounts left = countLeft(shot);
        const HalfCounts right = countRight(shot);
        const double leftRed = left.count ? static_cast<double>(left.red) / left.count : 0.0;
        const double leftBlue = left.count ? static_cast<double>(left.blue) / left.count : 0.0;
        const double rightRed = right.count ? static_cast<double>(right.red) / right.count : 0.0;
        const double rightBlue = right.count ? static_cast<double>(right.blue) / right.count : 0.0;
        // Lighting shifts the absolute levels, so judge the colour *direction*:
        // the visible front hemisphere runs u=0..0.5, where the RBRB map is
        // red for u=0..0.25 (screen-left half) and blue for u=0.25..0.5
        // (screen-right half). Compare blue-minus-red on each side; the right
        // half of the screen must be bluer than the left (larger B-R). A
        // left/right mirror inverts the quarters and flips the verdict.
        const double leftBlueMinusRed = leftBlue - leftRed;
        const double rightBlueMinusRed = rightBlue - rightRed;
        std::printf("ORIENT left  R=%.0f B=%.0f (B-R=%.0f)\n", leftRed, leftBlue, leftBlueMinusRed);
        std::printf("ORIENT right R=%.0f B=%.0f (B-R=%.0f)\n", rightRed, rightBlue, rightBlueMinusRed);
        const bool rightBluer = rightBlueMinusRed > leftBlueMinusRed;
        std::printf("ORIENT %s (expect right bluer than left = not mirrored)\n",
                    rightBluer ? "PASS left/right correct" : "FAIL mirrored");
        std::fflush(stdout);
        app.quit();
    });

    return app.exec();
}
