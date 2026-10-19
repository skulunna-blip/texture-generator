// Preview shape test: renders the material preview as a sphere and as a cube
// and checks that the cube is actually a cube.
//
// What this catches that a screenshot alone would not:
//   * a cube whose faces are wound the wrong way (back-face culling then hides
//     the outside and the shape reads as an empty silhouette),
//   * a cube whose faces are sized or offset wrong (the silhouette is not a
//     hexagon of the expected area),
//   * a stale geometry buffer still holding the sphere after the switch.
#include "MaterialPreviewWidget.h"

#include <QApplication>
#include <QImage>
#include <QMap>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QTimer>

#include <cmath>
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
    image.fill(qRgb(128, 128, 255));
    return image;
}

// Fraction of pixels in the frame that differ noticeably from the clear colour
// (0.06, 0.08, 0.11 -> about gray 20). The background is filled by glClear, so
// this is the covered-area measurement for the shape's silhouette.
double coverage(const QImage& shot)
{
    const QRgb background = qRgb(20, 24, 31);
    long covered = 0;
    for (int y = 0; y < shot.height(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            const QRgb pixel = shot.pixel(x, y);
            const int distance = std::abs(qRed(pixel) - qRed(background))
                + std::abs(qGreen(pixel) - qGreen(background))
                + std::abs(qBlue(pixel) - qBlue(background));
            if (distance > 24) {
                ++covered;
            }
        }
    }
    return static_cast<double>(covered) / static_cast<double>(shot.width() * shot.height());
}

// Coverage of the middle of the frame, where a convex closed shape must never
// show background. A missing face (a quad placed on the far side of the cube
// from its own normal, which back-face culling then removes) shows up here as a
// large dark patch even though the overall silhouette still looks plausible.
double centreCoverage(const QImage& shot)
{
    const QRgb background = qRgb(20, 24, 31);
    const int x0 = shot.width() / 4;
    const int y0 = shot.height() / 4;
    const int x1 = shot.width() - x0;
    const int y1 = shot.height() - y0;
    long covered = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const QRgb pixel = shot.pixel(x, y);
            const int distance = std::abs(qRed(pixel) - qRed(background))
                + std::abs(qGreen(pixel) - qGreen(background))
                + std::abs(qBlue(pixel) - qBlue(background));
            if (distance > 24) {
                ++covered;
            }
        }
    }
    return static_cast<double>(covered) / static_cast<double>((x1 - x0) * (y1 - y0));
}

// Distance from the centre to the outermost covered pixel, per direction. A
// cube is wider at its facets than at its corners; a sphere is equidistant in
// every direction. Comparing the spread of radii is a cheap shape test that
// does not depend on lighting.
double radiusSpread(const QImage& shot)
{
    const QRgb background = qRgb(20, 24, 31);
    const int cx = shot.width() / 2;
    const int cy = shot.height() / 2;
    double minRadius = 1e9;
    double maxRadius = 0.0;
    for (int angle = 0; angle < 360; angle += 5) {
        const double radians = angle * 3.14159265358979 / 180.0;
        const double dx = std::cos(radians);
        const double dy = std::sin(radians);
        double lastRadius = 0.0;
        for (double r = 0.0; r < shot.width() / 2.0; r += 1.0) {
            const int x = cx + static_cast<int>(dx * r);
            const int y = cy + static_cast<int>(dy * r);
            if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) {
                break;
            }
            const QRgb pixel = shot.pixel(x, y);
            const int distance = std::abs(qRed(pixel) - qRed(background))
                + std::abs(qGreen(pixel) - qGreen(background))
                + std::abs(qBlue(pixel) - qBlue(background));
            if (distance > 24) {
                lastRadius = r;
            }
        }
        minRadius = std::min(minRadius, lastRadius);
        maxRadius = std::max(maxRadius, lastRadius);
    }
    return maxRadius > 0.0 ? (maxRadius - minRadius) / maxRadius : 0.0;
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
        std::printf("SHAPE\n");
        std::fflush(stdout);

        // Freeze the turntable so both shots are taken from the same angle.
        const QPoint centre(256, 256);
        QMouseEvent press(QEvent::MouseButtonPress, centre, widget.mapToGlobal(centre),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);

        widget.setPreviewShape(MaterialPreviewWidget::PreviewShape::Sphere);
        QApplication::processEvents();
        const QImage sphereShot = widget.grabFramebuffer();
        sphereShot.save(QStringLiteral("shape_sphere.png"));
        const double sphereCoverage = coverage(sphereShot);
        const double sphereSpread = radiusSpread(sphereShot);

        widget.setPreviewShape(MaterialPreviewWidget::PreviewShape::Cube);
        // Same view as the sphere: the widget starts near-straight-on, which is
        // where the angularity comparison means something. resetView() (the
        // three-quarter angle the UI uses for cubes) is exercised separately.
        QApplication::processEvents();
        const QImage cubeShot = widget.grabFramebuffer();
        cubeShot.save(QStringLiteral("shape_cube.png"));
        const double cubeCoverage = coverage(cubeShot);
        const double cubeSpread = radiusSpread(cubeShot);
        const double cubeCentre = centreCoverage(cubeShot);

        // The angled view the UI actually uses for the cube.
        widget.resetView();
        QApplication::processEvents();
        const QImage angledShot = widget.grabFramebuffer();
        angledShot.save(QStringLiteral("shape_cube_angled.png"));
        const double angledCoverage = coverage(angledShot);
        const double angledSpread = radiusSpread(angledShot);
        const double angledCentre = centreCoverage(angledShot);

        QMouseEvent release(QEvent::MouseButtonRelease, centre, widget.mapToGlobal(centre),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &release);

        std::printf("sphere coverage=%.3f  radius_spread=%.3f\n", sphereCoverage, sphereSpread);
        std::printf("cube   coverage=%.3f  radius_spread=%.3f\n", cubeCoverage, cubeSpread);
        std::printf("cube@  coverage=%.3f  radius_spread=%.3f  (three-quarter view)\n",
                    angledCoverage, angledSpread);

        // 1. The cube must actually be drawn. A wrong-wound or missing cube
        //    leaves only the background, so coverage collapses.
        const bool cubeDrawn = cubeCoverage > 0.12;
        std::printf("cube is drawn                        %s  coverage %.3f\n",
                    cubeDrawn ? "PASS" : "FAIL", cubeCoverage);

        // 2. A straight-on cube is a square (spread ~0) whose top and bottom
        //    edges recede, so the silhouette is still less circular than the
        //    sphere's. A three-quarter view is
        //    a hexagon whose corners are closer to the centre than its facet
        //    edges. A sphere is close to circular in every direction. The view
        //    here is the idle turntable's, which is straight-on for both along
        //    Y, so the cube must be *more* angular than the sphere.
        const bool cubeMoreAngular = cubeSpread > sphereSpread + 0.02;
        std::printf("cube is more angular than the sphere %s  cube %.3f vs sphere %.3f\n",
                    cubeMoreAngular ? "PASS" : "FAIL", cubeSpread, sphereSpread);

        // 3. Switching back must restore the sphere, i.e. the geometry buffer
        //    is rebuilt rather than left holding the cube.
        widget.setPreviewShape(MaterialPreviewWidget::PreviewShape::Sphere);
        QApplication::processEvents();
        const QImage backShot = widget.grabFramebuffer();
        const double backCoverage = coverage(backShot);
        const double backSpread = radiusSpread(backShot);
        const bool restored = std::abs(backSpread - sphereSpread) < 0.05
            && std::abs(backCoverage - sphereCoverage) < 0.05;
        std::printf("switching back restores the sphere   %s  coverage %.3f spread %.3f\n",
                    restored ? "PASS" : "FAIL", backCoverage, backSpread);

        // 4. The two shapes must not cover the same area: a cube of half-size
        //    0.9 seen straight-on is smaller than a unit sphere.
        const bool sizesDiffer = std::abs(cubeCoverage - sphereCoverage) > 0.01;
        std::printf("the shapes are distinguishable      %s  %.3f vs %.3f\n",
                    sizesDiffer ? "PASS" : "FAIL", cubeCoverage, sphereCoverage);

        // 5. Three-quarter view: the cube's silhouette must be a hexagon with
        //    two facets visible. This is the view the UI switches to, and it is
        //    where a wrong camera distance shows up as a cube that overflows
        //    the frame (the corners hit the near plane).
        const bool angledVisible = angledCoverage > 0.12 && angledCoverage < 0.95;
        std::printf("three-quarter view stays in frame   %s  coverage %.3f\n",
                    angledVisible ? "PASS" : "FAIL", angledCoverage);

        // 6. No holes: a closed convex solid covers the centre completely from
        //    every angle. A face built on the wrong side of the cube is culled
        //    away and leaves a straight-edged hole that never touches the
        //    silhouette, so the coverage/spread checks above cannot see it.
        const bool noHoles = cubeCentre > 0.999 && angledCentre > 0.999;
        std::printf("cube has no missing faces         %s  centre %.3f / %.3f\n",
                    noHoles ? "PASS" : "FAIL", cubeCentre, angledCentre);

        const bool allOk = cubeDrawn && cubeMoreAngular && restored && sizesDiffer
            && angledVisible && noHoles;
        std::printf("tg_shape_check: %s\n", allOk ? "ALL PASS" : "FAILURES");
        std::fflush(stdout);
        app.exit(allOk ? 0 : 1);
    });

    return app.exec();
}
