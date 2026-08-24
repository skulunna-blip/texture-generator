#include "TiledPreviewWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
QSize scaledTileSize(const QSize& source, const QSize& bounds)
{
    if (!source.isValid() || !bounds.isValid()) {
        return QSize();
    }

    QSize size = source;
    size.scale(bounds, Qt::KeepAspectRatio);
    return size;
}

// clamp to powers-friendly range; keep the animation comfortable
constexpr double kMinZoom = 0.25;
constexpr double kMaxZoom = 8.0;
}

TiledPreviewWidget::TiledPreviewWidget(QWidget* parent)
    : QWidget(parent)
    , m_placeholderText(QStringLiteral("Tiled Preview"))
{
    setMinimumSize(220, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
}

void TiledPreviewWidget::setImage(const QImage& image)
{
    m_image = image;
    resetView();
    update();
}

void TiledPreviewWidget::setPlaceholderText(const QString& text)
{
    m_placeholderText = text;
    update();
}

void TiledPreviewWidget::resetView()
{
    m_zoom = 1.0;
    m_pan = QPointF(0.0, 0.0);
    m_panning = false;
}

void TiledPreviewWidget::zoomAt(const QPointF& widgetPos, double factor)
{
    if (m_image.isNull()) {
        return;
    }

    const double newZoom = std::clamp(m_zoom * factor, kMinZoom, kMaxZoom);
    const double applied = newZoom / m_zoom;
    m_zoom = newZoom;

    // Zoom about the cursor: the content point under the cursor must stay put.
    // The pattern centre sits at contentRect centre + m_pan; scaling the pattern
    // about the cursor means moving the pan so the cursor stays anchored.
    const QPointF centre = rect().center() + m_pan;
    const QPointF offset = widgetPos - centre;
    m_pan += offset * (1.0 - applied);
}

void TiledPreviewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_image.isNull()) {
        return;
    }

    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        event->accept();
        return;
    }

    const double factor = std::pow(1.25, steps);
    zoomAt(event->position(), factor);
    update();
    event->accept();
}

void TiledPreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panStart = event->position();
        m_panStartOrigin = m_pan;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TiledPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning && (event->buttons() & Qt::MiddleButton)) {
        const QPointF delta = event->position() - m_panStart;
        m_pan = m_panStartOrigin + delta;
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TiledPreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TiledPreviewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        resetView();
        update();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TiledPreviewWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(15, 20, 28));

    const QRectF contentRect = rect().adjusted(14, 14, -14, -14);
    QPainterPath clipPath;
    clipPath.addRoundedRect(contentRect, 8.0, 8.0);
    painter.save();
    painter.setClipPath(clipPath);

    painter.fillRect(contentRect, QColor(18, 25, 35));

    if (m_image.isNull()) {
        painter.restore();
        painter.setPen(QColor(145, 162, 205));
        painter.drawText(rect(), Qt::AlignCenter, m_placeholderText);
        return;
    }

    const int gap = 8;
    const QSize cellBounds(static_cast<int>((contentRect.width() - gap * 2) / 3.0),
                           static_cast<int>((contentRect.height() - gap * 2) / 3.0));
    const QSize fitTile = scaledTileSize(m_image.size(), cellBounds);
    if (!fitTile.isValid()) {
        painter.restore();
        return;
    }

    const double tileWidth = fitTile.width() * m_zoom;
    const double tileHeight = fitTile.height() * m_zoom;
    const QImage smooth = m_image.scaled(static_cast<int>(std::ceil(tileWidth)),
                                         static_cast<int>(std::ceil(tileHeight)),
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);

    // Pattern is 3x3 tiles, top-left of tile (0,0) anchored at
    // contentRect centre + m_pan, then shifted so the 3x3 grid is centred.
    const double patternWidth = tileWidth * 3 + gap * 2;
    const double patternHeight = tileHeight * 3 + gap * 2;
    const double startX = contentRect.center().x() + m_pan.x() - patternWidth / 2.0;
    const double startY = contentRect.center().y() + m_pan.y() - patternHeight / 2.0;

    // When zoomed in far, the whole 3x3 pattern can exceed the view; we still
    // draw all nine tiles and let the clip region cull.
    painter.setOpacity(0.94);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const QRectF target(startX + column * (tileWidth + gap),
                                startY + row * (tileHeight + gap),
                                tileWidth,
                                tileHeight);
            painter.drawImage(target, smooth, QRectF(0, 0, smooth.width(), smooth.height()));
        }
    }
    painter.restore();

    painter.setPen(QPen(QColor(58, 68, 84), 1));
    painter.drawRoundedRect(contentRect, 8.0, 8.0);

    // Zoom readout so the interaction is discoverable.
    if (std::abs(m_zoom - 1.0) > 0.01) {
        painter.setPen(QColor(145, 162, 205));
        painter.drawText(QRectF(contentRect.x() + 12, contentRect.y() + 8, 120, 20),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1x").arg(m_zoom, 0, 'f', 2));
    }
}
