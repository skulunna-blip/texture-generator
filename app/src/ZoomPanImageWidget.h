#pragma once

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QPixmap;
class QWheelEvent;

// A single-image viewer with mouse-wheel zoom (anchored at the cursor) and
// middle-button pan, used for the per-channel map preview. No image means the
// placeholder text is drawn instead.
class ZoomPanImageWidget : public QWidget
{
public:
    explicit ZoomPanImageWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    QImage image() const;
    void setPlaceholderText(const QString& text);
    void clear();
    void resetView();

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QImage m_image;
    QPixmap* m_pixmap = nullptr;
    QString m_placeholderText;
    double m_zoom = 1.0;
    QPointF m_offset;
    bool m_panning = false;
    QPoint m_lastPos;
};
