#include "ZoomPanImageWidget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 16.0;

// Scale that fits the whole image inside the widget.
double fitScale(const QImage& image, int widgetWidth, int widgetHeight)
{
    if (image.isNull() || widgetWidth <= 0 || widgetHeight <= 0) {
        return 1.0;
    }
    const double sx = static_cast<double>(widgetWidth) / static_cast<double>(image.width());
    const double sy = static_cast<double>(widgetHeight) / static_cast<double>(image.height());
    return std::min(sx, sy);
}
} // namespace

ZoomPanImageWidget::ZoomPanImageWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(280, 280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(false);
}

void ZoomPanImageWidget::setImage(const QImage& image)
{
    m_image = image;
    delete m_pixmap;
    m_pixmap = image.isNull() ? nullptr : new QPixmap(QPixmap::fromImage(image));
    m_zoom = 1.0;
    m_offset = QPointF();
    update();
}

QImage ZoomPanImageWidget::image() const
{
    return m_image;
}

void ZoomPanImageWidget::setPlaceholderText(const QString& text)
{
    m_placeholderText = text;
    update();
}

void ZoomPanImageWidget::clear()
{
    setImage(QImage());
    m_placeholderText.clear();
}

void ZoomPanImageWidget::resetView()
{
    m_zoom = 1.0;
    m_offset = QPointF();
    update();
}

void ZoomPanImageWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(13, 18, 24));

    if (!m_pixmap || m_image.isNull()) {
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(QColor(145, 162, 205));
        painter.drawText(rect(), Qt::AlignCenter, m_placeholderText);
        return;
    }

    const double s = fitScale(m_image, width(), height()) * m_zoom;
    const double scaledWidth = m_image.width() * s;
    const double scaledHeight = m_image.height() * s;
    const double x = (width() - scaledWidth) / 2.0 + m_offset.x();
    const double y = (height() - scaledHeight) / 2.0 + m_offset.y();

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawPixmap(QRectF(x, y, scaledWidth, scaledHeight), *m_pixmap, QRectF(m_pixmap->rect()));
}

void ZoomPanImageWidget::wheelEvent(QWheelEvent* event)
{
    if (m_image.isNull()) {
        event->ignore();
        return;
    }

    // Zoom anchored at the cursor: keep the image point under the mouse fixed.
    const double factor = std::pow(1.15, static_cast<double>(event->angleDelta().y()) / 120.0);
    const double newZoom = std::clamp(m_zoom * factor, kMinZoom, kMaxZoom);
    if (newZoom == m_zoom) {
        event->accept();
        return;
    }

    const double s = fitScale(m_image, width(), height()) * m_zoom;
    const double s2 = fitScale(m_image, width(), height()) * newZoom;
    const QPointF mouse(event->position());
    const QPointF center(width() / 2.0, height() / 2.0);
    const QPointF imagePos = (mouse - center - m_offset) / s;

    m_zoom = newZoom;
    m_offset = mouse - center - imagePos * s2;
    update();
    event->accept();
}

void ZoomPanImageWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && !m_image.isNull()) {
        m_panning = true;
        m_lastPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ZoomPanImageWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        m_offset += QPointF(event->pos() - m_lastPos);
        m_lastPos = event->pos();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ZoomPanImageWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ZoomPanImageWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && !m_image.isNull()) {
        resetView();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
