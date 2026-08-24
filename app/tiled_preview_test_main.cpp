// Interaction check for TiledPreviewWidget: wheel zoom, middle-drag pan, double
// click reset. Runs headless with a QApplication and synthesized events.
#include "TiledPreviewWidget.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cstdio>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QImage image(128, 128, QImage::Format_RGB32);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            image.setPixel(x, y, ((x / 32 + y / 32) % 2) ? qRgb(200, 80, 80) : qRgb(40, 40, 160));
        }
    }

    TiledPreviewWidget widget;
    widget.resize(400, 300);
    widget.setImage(image);
    widget.show();

    // 1. Wheel up -> zoom in (zoom grows from 1.0).
    QWheelEvent wheelUp(QPointF(200, 150), QPoint(200, 150),
                      QPoint(0, 0), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &wheelUp);
    QApplication::processEvents();

    // The zoom state is private; assert via painting: after 2 more wheel-ups the
    // zoom readout ("N.NNx") appears in the top-left corner of the content area.
    for (int i = 0; i < 2; ++i) {
        QWheelEvent w(QPointF(200, 150), QPoint(200, 150),
                      QPoint(0, 0), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
        QApplication::sendEvent(&widget, &w);
        QApplication::processEvents();
    }

    QImage shot = widget.grab().toImage();
    bool zoomLabel = false;
    // After zoom != 1 the widget draws a "N.NNx" readout in the top-left corner
    // of the content area (light blue on the dark background).
    for (int y = 20; y < 42; y += 2) {
        for (int x = 24; x < 150; x += 2) {
            const QRgb p = shot.pixel(x, y);
            const int b = qBlue(p);
            if (b > 120 && qRed(p) > 100) {
                zoomLabel = true;
            }
        }
    }
    std::printf("ZOOM labelPresent=%s (expect yes after wheel-up)\n",
                zoomLabel ? "yes" : "no");

    // 2. Middle-drag pan changes the view (two grabs differ).
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(200, 150), Qt::MiddleButton,
                      Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(220, 170), Qt::NoButton,
                     Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    QApplication::processEvents();
    const QImage panned = widget.grab().toImage();

    long diff = 0;
    for (int y = 0; y < shot.height(); y += 8) {
        for (int x = 0; x < shot.width(); x += 8) {
            diff += std::abs(qGray(shot.pixel(x, y)) - qGray(panned.pixel(x, y)));
        }
    }
    std::printf("PAN frameDiff=%ld (expect > 0 after middle-drag)\n", diff);

    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(220, 170), Qt::MiddleButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
    QApplication::processEvents();

    // 3. Double click resets zoom back to fit: the corner no longer shows a big
    // bright tile (fit keeps the checker small near the widget edge).
    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(200, 150), Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &dbl);
    QApplication::processEvents();
    const QImage reset = widget.grab().toImage();
    const QRgb pReset = reset.pixel(6, 6);
    std::printf("RESET cornerGray=%d (expect dark background ~18-35 after reset)\n", qGray(pReset));

    std::fflush(stdout);
    return 0;
}
