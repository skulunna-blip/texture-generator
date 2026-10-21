#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMap>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QTimer>

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

class MaterialPreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    enum class PreviewMode
    {
        Full,        // everything as generated
        NormalOnly,  // the normal map's own RGB, unlit, as CrazyBump/PS show it
        BumpOnly     // plain surface lit only by bump-derived normals
    };

    // The shape the material is wrapped around. A sphere shows how a map reads
    // from every angle; a cube shows how it tiles across flat faces and keeps
    // its orientation legible, which is what most texture artists judge by.
    enum class PreviewShape
    {
        Sphere,
        Cube
    };

    explicit MaterialPreviewWidget(QWidget* parent = nullptr);
    ~MaterialPreviewWidget() override;

    void setMaps(const QMap<QString, QImage>& images);
    void clearMaps();
    void setPlaceholderText(const QString& text);
    void setPreviewMode(PreviewMode mode);
    PreviewMode previewMode() const;
    void setPreviewShape(PreviewShape shape);
    PreviewShape previewShape() const;
    void resetView();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void ensureProgram();
    void ensureGeometry();
    void rebuildTextures();
    void destroyTextures();
    void bindTexture(int unit, QOpenGLTexture* texture, const char* uniformName);
    void checkGlError(const char* tag);

    QMap<QString, QImage> m_images;
    QString m_placeholderText;
    PreviewMode m_previewMode = PreviewMode::Full;
    PreviewShape m_shape = PreviewShape::Sphere;
    QOpenGLShaderProgram* m_program = nullptr;
    bool m_glReady = false;
    bool m_programFailed = false;
    QOpenGLBuffer m_vertexBuffer;
    QOpenGLBuffer m_indexBuffer;
    QOpenGLVertexArrayObject m_vao;
    int m_indexCount = 0;
    PreviewShape m_builtShape = PreviewShape::Sphere;
    bool m_texturesDirty = true;

    QOpenGLTexture* m_baseTexture = nullptr;
    QOpenGLTexture* m_bumpTexture = nullptr;
    QOpenGLTexture* m_normalTexture = nullptr;
    QOpenGLTexture* m_roughnessTexture = nullptr;
    QOpenGLTexture* m_metalTexture = nullptr;
    QOpenGLTexture* m_aoTexture = nullptr;
    QOpenGLTexture* m_alphaTexture = nullptr;

    QTimer m_frameTimer;
    QElapsedTimer m_elapsed;
    QPoint m_lastMousePos;
    float m_pitch = 0.0f;
    float m_manualYaw = 0.0f;
    float m_distance = 2.8f;
    float m_spinOffset = 0.0f;
    float m_lastSpinTime = 0.0f;
    bool m_dragging = false;
};
