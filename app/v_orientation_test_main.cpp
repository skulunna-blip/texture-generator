// Vertical-orientation check for the material preview.
//
// The sphere and the cube build their UVs differently: the sphere puts v=0 at
// the north pole (so the image's row 0 lands on top), the cube puts v=1 at the
// top edge. Both feed the same shader, and makeTexture does not mirror, so only
// one of them can be showing the image the right way up. This test renders a
// map that is unmistakable - red on the image's top rows, blue on its bottom -
// on each shape and reports which way up it lands.
#include "MaterialPreviewWidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QMap>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
QImage topRedBottomBlue(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        const QRgb colour = (y < size * 45 / 100) ? qRgb(235, 40, 40) : qRgb(40, 50, 235);
        for (int x = 0; x < size; ++x) {
            image.setPixel(x, y, colour);
        }
    }
    return image;
}

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

struct Region
{
    double redBlue = 0.0; // mean(R) - mean(B): positive = the red top rows won
    double luma = 0.0;
    long count = 0;
};

Region sample(const QImage& shot, int x0, int x1, int y0, int y1)
{
    Region r;
    double red = 0.0;
    double blue = 0.0;
    double luma = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const QRgb p = shot.pixel(x, y);
            red += qRed(p);
            blue += qBlue(p);
            luma += 0.2126 * qRed(p) + 0.7152 * qGreen(p) + 0.0722 * qBlue(p);
            ++r.count;
        }
    }
    if (r.count > 0) {
        r.redBlue = (red - blue) / r.count;
        r.luma = luma / r.count;
    }
    return r;
}

// Renders one shape with one map set and returns the frame buffer.
QImage render(MaterialPreviewWidget::PreviewShape shape, const QMap<QString, QImage>& maps,
              MaterialPreviewWidget::PreviewMode mode, int size)
{
    MaterialPreviewWidget widget;
    widget.resize(size, size);
    widget.setPreviewMode(mode);
    widget.setPreviewShape(shape);
    widget.setMaps(maps);
    widget.show();

    // Hold the left button so the turntable never advances: yaw and pitch stay
    // at zero and every run sees the same pose.
    QMouseEvent press(QEvent::MouseButtonPress, QPoint(size / 2, size / 2),
                      widget.mapToGlobal(QPoint(size / 2, size / 2)), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);

    QImage shot;
    const int delay = 700;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(delay);
    QEventLoop loop;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();
    shot = widget.grabFramebuffer();
    return shot;
}

void checkShape(const char* name, MaterialPreviewWidget::PreviewShape shape)
{
    constexpr int size = 512;
    QMap<QString, QImage> maps;
    maps.insert(QStringLiteral("base"), topRedBottomBlue(256));
    maps.insert(QStringLiteral("bump"), gray(256, 128));
    QImage flat(256, 256, QImage::Format_RGB32);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            flat.setPixel(x, y, qRgb(128, 128, 255));
        }
    }
    maps.insert(QStringLiteral("normal"), flat);
    maps.insert(QStringLiteral("roughness"), gray(256, 96));
    maps.insert(QStringLiteral("metal"), gray(256, 0));
    maps.insert(QStringLiteral("ao"), gray(256, 255));
    maps.insert(QStringLiteral("alpha"), gray(256, 255));

    const QImage shot = render(shape, maps, MaterialPreviewWidget::PreviewMode::Full, size);
    shot.save(QStringLiteral("vorient_%1.png").arg(QString::fromLatin1(name)));

    // Sample inside the object: the middle band of columns, upper and lower
    // quarters of the frame. Both shapes fill that area at the default zoom.
    const int cx = size / 2;
    const int cy = size / 2;
    const int halfW = size / 7;
    const int band = size / 8;
    const Region top = sample(shot, cx - halfW, cx + halfW, cy - band * 2, cy - band);
    const Region bottom = sample(shot, cx - halfW, cx + halfW, cy + band, cy + band * 2);
    std::printf("VORIENT %-6s top(R-B)=%6.1f luma=%5.1f | bottom(R-B)=%6.1f luma=%5.1f | %s\n",
                name, top.redBlue, top.luma, bottom.redBlue, bottom.luma,
                top.redBlue > bottom.redBlue ? "image TOP is up" : "image TOP is DOWN");
}

// A Gaussian bump, the least ambiguous height cue there is.
QImage bumpHeight(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    const double cx = size / 2.0;
    const double cy = size / 2.0;
    const double sigma = size * 0.12;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double r2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            const int v = static_cast<int>(255.0 * std::exp(-r2 / (2.0 * sigma * sigma)));
            image.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return image;
}

// The generator's formula (TextureGenerator::createNormalMap), copied so the
// test can hold the shader to it without linking the whole generator.
QImage normalFromHeight(const QImage& height, double gain)
{
    const int w = height.width();
    const int h = height.height();
    auto sample = [&](int x, int y) {
        return static_cast<double>(qRed(height.pixel(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1))));
    };
    QImage out(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double gx = (sample(x + 1, y - 1) + 2.0 * sample(x + 1, y) + sample(x + 1, y + 1))
                      - (sample(x - 1, y - 1) + 2.0 * sample(x - 1, y) + sample(x - 1, y + 1));
            double gy = (sample(x - 1, y + 1) + 2.0 * sample(x, y + 1) + sample(x + 1, y + 1))
                      - (sample(x - 1, y - 1) + 2.0 * sample(x, y - 1) + sample(x + 1, y - 1));
            gx /= 4.0;
            gy /= 4.0;
            double nx = -gx * gain;
            double ny = gy * gain;
            const double len = std::sqrt(nx * nx + ny * ny + 1.0);
            nx /= len;
            ny /= len;
            const double nz = 1.0 / len;
            out.setPixel(x, y, qRgb(static_cast<int>((nx * 0.5 + 0.5) * 255.0),
                                    static_cast<int>((ny * 0.5 + 0.5) * 255.0),
                                    static_cast<int>((nz * 0.5 + 0.5) * 255.0)));
        }
    }
    return out;
}

// The height-map path and the normal-map path must agree: a raised bump has to
// catch the light on the same side whichever way the material tells the shader
// about it. If they disagree the preview is contradicting itself, and one of
// them is inverted relative to Blender.
void checkBumpConsistency()
{
    constexpr int size = 512;
    const QImage height = bumpHeight(256);
    QImage flatNormal(256, 256, QImage::Format_RGB32);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            flatNormal.setPixel(x, y, qRgb(128, 128, 255));
        }
    }

    QMap<QString, QImage> bumpOnlyMaps;
    bumpOnlyMaps.insert(QStringLiteral("base"), gray(256, 190));
    bumpOnlyMaps.insert(QStringLiteral("bump"), height);
    bumpOnlyMaps.insert(QStringLiteral("normal"), flatNormal);
    bumpOnlyMaps.insert(QStringLiteral("roughness"), gray(256, 96));
    bumpOnlyMaps.insert(QStringLiteral("metal"), gray(256, 0));
    bumpOnlyMaps.insert(QStringLiteral("ao"), gray(256, 255));
    bumpOnlyMaps.insert(QStringLiteral("alpha"), gray(256, 255));

    QMap<QString, QImage> normalMaps = bumpOnlyMaps;
    normalMaps.insert(QStringLiteral("normal"), normalFromHeight(height, 0.08));

    const QImage bumpShot = render(MaterialPreviewWidget::PreviewShape::Cube, bumpOnlyMaps,
                                   MaterialPreviewWidget::PreviewMode::BumpOnly, size);
    const QImage normalShot = render(MaterialPreviewWidget::PreviewShape::Cube, normalMaps,
                                     MaterialPreviewWidget::PreviewMode::Full, size);
    bumpShot.save(QStringLiteral("vorient_bump.png"));
    normalShot.save(QStringLiteral("vorient_normal.png"));

    const int cx = size / 2;
    const int cy = size / 2;
    const int halfW = size / 14;
    const int off = size / 20;
    const Region bumpUp = sample(bumpShot, cx - halfW, cx + halfW, cy - off * 3, cy - off);
    const Region bumpDown = sample(bumpShot, cx - halfW, cx + halfW, cy + off, cy + off * 3);
    const Region normUp = sample(normalShot, cx - halfW, cx + halfW, cy - off * 3, cy - off);
    const Region normDown = sample(normalShot, cx - halfW, cx + halfW, cy + off, cy + off * 3);
    const double bumpDelta = bumpUp.luma - bumpDown.luma;
    const double normalDelta = normUp.luma - normDown.luma;
    std::printf("BUMPCONS bump-only dLuma=%6.1f  normal-map dLuma=%6.1f  %s\n", bumpDelta, normalDelta,
                (bumpDelta > 0) == (normalDelta > 0) ? "PASS agree" : "FAIL disagree");
}

} // namespace

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);

    checkShape("sphere", MaterialPreviewWidget::PreviewShape::Sphere);
    checkShape("cube", MaterialPreviewWidget::PreviewShape::Cube);
    checkBumpConsistency();

    std::fflush(stdout);
    return 0;
}
