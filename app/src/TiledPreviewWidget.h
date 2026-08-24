#pragma once

#include <QImage>
#include <QPointF>
#include <QString>
#include <QWidget>

class QMouseEvent;
class QWheelEvent;

class TiledPreviewWidget : public QWidget
{
public:
    explicit TiledPreviewWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setPlaceholderText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void resetView();
    void zoomAt(const QPointF& widgetPos, double factor);

    QImage m_image;
    QString m_placeholderText;
    double m_zoom = 1.0;
    QPointF m_pan;
    bool m_panning = false;
    QPointF m_panStart;
    QPointF m_panStartOrigin;
};
