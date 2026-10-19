#include "MaterialPreviewWidget.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPaintEvent>
#include <QSurfaceFormat>
#include <QVector2D>
#include <QVector3D>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kPi = 3.1415926535f;

// The cube's face diagonal (|(±h, ±h, ±h)| with h = 0.9) is 1.56, against the
// sphere's radius of 1.0, so at the sphere's camera distance the cube's corners
// run into the near plane and the shape stops reading as a cube. Pulling the
// camera back for the cube keeps its whole silhouette inside the frame.
float defaultDistance(MaterialPreviewWidget::PreviewShape shape)
{
    return shape == MaterialPreviewWidget::PreviewShape::Cube ? 4.6f : 2.8f;
}

// Keep a rotation angle inside [lower, upper) so a material ball can be spun
// through a full 360 degrees without the angle growing without bound.
float wrapDegrees(float angle, float lower, float upper)
{
    const float range = upper - lower;
    angle = std::fmod(angle - lower, range);
    if (angle < 0.0f) {
        angle += range;
    }
    return lower + angle;
}

QString glLogPath()
{
    return QDir::temp().filePath(QStringLiteral("tg_preview_gl.log"));
}

void logGl(const QString& message)
{
    QFile file(glLogPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(QStringLiteral("[%1] %2\n")
                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                       .arg(message)
                       .toUtf8());
        file.close();
    }
    qDebug().noquote() << message;
}

struct PreviewVertex
{
    float px;
    float py;
    float pz;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
    float tx;
    float ty;
    float tz;
};

QOpenGLTexture* makeTexture(const QImage& image)
{
    if (image.isNull()) {
        return nullptr;
    }

    // Do NOT mirror: QOpenGLTexture keeps row 0 of the QImage at v=0, which is
    // the top of the sphere (v=0 is the north pole in appendSphere). Mirroring
    // here would put the texture's bottom half on the ball's top and read as a
    // vertically flipped material.
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    auto* texture = new QOpenGLTexture(rgba, QOpenGLTexture::GenerateMipMaps);
    texture->setWrapMode(QOpenGLTexture::Repeat);
    texture->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
    texture->setMagnificationFilter(QOpenGLTexture::Linear);
    return texture;
}

void appendSphere(QVector<PreviewVertex>& vertices, QVector<quint32>& indices)
{
    constexpr int slices = 72;
    constexpr int stacks = 48;

    vertices.reserve((slices + 1) * (stacks + 1));
    indices.reserve(slices * stacks * 6);

    for (int stack = 0; stack <= stacks; ++stack) {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks);
        const float phi = v * kPi;
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);

        for (int slice = 0; slice <= slices; ++slice) {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * 2.0f * kPi;
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            const QVector3D position(-sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
            // The X is negated so u=0 sits at -X, matching Blender's default UV
            // sphere (u=0 seam on the left). Without this the map is mirrored
            // left/right relative to Blender. The tangent follows the mirrored
            // u direction so normal maps stay consistent with the surface.
            QVector3D tangent(sinTheta, 0.0f, cosTheta);
            if (tangent.lengthSquared() < 1.0e-5f) {
                tangent = QVector3D(1.0f, 0.0f, 0.0f);
            }
            tangent.normalize();

            vertices.push_back({
                position.x(),
                position.y(),
                position.z(),
                position.x(),
                position.y(),
                position.z(),
                u,
                v,
                tangent.x(),
                tangent.y(),
                tangent.z(),
            });
        }
    }

    for (int stack = 0; stack < stacks; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            const quint32 row0 = static_cast<quint32>(stack * (slices + 1) + slice);
            const quint32 row1 = static_cast<quint32>((stack + 1) * (slices + 1) + slice);
            indices.push_back(row0);
            indices.push_back(row1);
            indices.push_back(row0 + 1);
            indices.push_back(row0 + 1);
            indices.push_back(row1);
            indices.push_back(row1 + 1);
        }
    }
}

// One face of a cube, as a quad. Each face gets the same u/v range (0..1) so
// the whole generated map lands on every side, the way a tiling texture is
// normally judged. The tangent is the face's world-space +u axis, which the
// shader combines with the (flat, per-face) normal to build a valid TBN frame,
// so normal maps still resolve correctly on a flat surface.
void appendQuad(QVector<PreviewVertex>& vertices,
                QVector<quint32>& indices,
                const QVector3D& origin,
                const QVector3D& uAxis,
                const QVector3D& vAxis,
                const QVector3D& normal)
{
    const quint32 base = static_cast<quint32>(vertices.size());
    const QVector3D tangent = uAxis.normalized();

    const QVector3D corners[4] = {
        origin,
        origin + uAxis,
        origin + uAxis + vAxis,
        origin + vAxis,
    };
    const QVector2D uvs[4] = {
        QVector2D(0.0f, 0.0f),
        QVector2D(1.0f, 0.0f),
        QVector2D(1.0f, 1.0f),
        QVector2D(0.0f, 1.0f),
    };
    for (int corner = 0; corner < 4; ++corner) {
        vertices.push_back({
            corners[corner].x(),
            corners[corner].y(),
            corners[corner].z(),
            normal.x(),
            normal.y(),
            normal.z(),
            uvs[corner].x(),
            uvs[corner].y(),
            tangent.x(),
            tangent.y(),
            tangent.z(),
        });
    }

    // Counter-clockwise when seen from outside, so back-face culling keeps the
    // faces we want and hides the inside of the cube.
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

void appendCube(QVector<PreviewVertex>& vertices, QVector<quint32>& indices)
{
    // A half-size of 0.9 puts the cube's silhouette close to the sphere's
    // radius, so switching shapes does not make the material jump in size.
    constexpr float h = 0.9f;

    // +X, -X. The origin has to sit on the same side as the normal: a quad at
    // x = -h carrying a +X normal is an inward-facing face, which back-face
    // culling removes, leaving a hole where the outside of the cube should be.
    appendQuad(vertices, indices, {h, -h, h}, {0, 0, -2.0f * h}, {0, 2.0f * h, 0}, {1, 0, 0});
    appendQuad(vertices, indices, {-h, -h, -h}, {0, 0, 2.0f * h}, {0, 2.0f * h, 0}, {-1, 0, 0});
    // +Y, -Y
    appendQuad(vertices, indices, {-h, h, h}, {2.0f * h, 0, 0}, {0, 0, -2.0f * h}, {0, 1, 0});
    appendQuad(vertices, indices, {-h, -h, -h}, {2.0f * h, 0, 0}, {0, 0, 2.0f * h}, {0, -1, 0});
    // +Z, -Z
    appendQuad(vertices, indices, {-h, -h, h}, {2.0f * h, 0, 0}, {0, 2.0f * h, 0}, {0, 0, 1});
    appendQuad(vertices, indices, {h, -h, -h}, {-2.0f * h, 0, 0}, {0, 2.0f * h, 0}, {0, 0, -1});
}

const char* kVertexShader = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aTangent;

uniform mat4 uMvp;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out vec3 vTangent;
out vec3 vBitangent;

void main()
{
    vec3 normal = normalize(uNormalMatrix * aNormal);
    vec3 tangent = normalize(uNormalMatrix * aTangent);
    vec3 bitangent = normalize(cross(normal, tangent));

    vWorldPos = vec3(uModel * vec4(aPosition, 1.0));
    vNormal = normal;
    vTangent = tangent;
    vBitangent = bitangent;
    vUv = aUv;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)GLSL";

const char* kFragmentShader = R"GLSL(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
in vec3 vTangent;
in vec3 vBitangent;

uniform sampler2D uBaseTex;
uniform sampler2D uBumpTex;
uniform sampler2D uNormalTex;
uniform sampler2D uRoughnessTex;
uniform sampler2D uMetalTex;
uniform sampler2D uAoTex;
uniform sampler2D uAlphaTex;

uniform int uHasBase;
uniform int uHasBump;
uniform int uHasNormal;
uniform int uHasRoughness;
uniform int uHasMetal;
uniform int uHasAo;
uniform int uHasAlpha;
uniform int uViewMode; // 0 = full, 1 = normal only, 2 = bump only
uniform vec2 uBumpTexel;
uniform vec3 uLightDir;
uniform vec3 uFillDir;
uniform vec3 uCameraPos;

out vec4 fragColor;

float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / max(3.14159265 * denom * denom, 0.0001);
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 sampleSurfaceNormal()
{
    vec3 N = normalize(vNormal);
    mat3 TBN = mat3(normalize(vTangent), normalize(vBitangent), N);

    // Isolate a single channel: normal-only and bump-only ignore every other
    // map and pick their own source, so the user can judge each one's actual
    // contribution before looking at the full stack.
    if (uViewMode == 1) {
        if (uHasNormal == 1) {
            vec3 tangentNormal = texture(uNormalTex, vUv).xyz * 2.0 - 1.0;
            tangentNormal.y = -tangentNormal.y;
            return normalize(TBN * tangentNormal);
        }
        return N;
    }

    if (uViewMode == 2) {
        if (uHasBump == 1) {
            float hL = texture(uBumpTex, vUv - vec2(uBumpTexel.x, 0.0)).r;
            float hR = texture(uBumpTex, vUv + vec2(uBumpTexel.x, 0.0)).r;
            float hU = texture(uBumpTex, vUv - vec2(0.0, uBumpTexel.y)).r;
            float hD = texture(uBumpTex, vUv + vec2(0.0, uBumpTexel.y)).r;
            vec3 bumpNormal = normalize(vec3((hL - hR) * 2.6, (hU - hD) * 2.6, 1.0));
            return normalize(TBN * bumpNormal);
        }
        return N;
    }

    if (uHasNormal == 1) {
        vec3 tangentNormal = texture(uNormalTex, vUv).xyz * 2.0 - 1.0;
        tangentNormal.y = -tangentNormal.y;
        return normalize(TBN * tangentNormal);
    }

    if (uHasBump == 1) {
        float hL = texture(uBumpTex, vUv - vec2(uBumpTexel.x, 0.0)).r;
        float hR = texture(uBumpTex, vUv + vec2(uBumpTexel.x, 0.0)).r;
        float hU = texture(uBumpTex, vUv - vec2(0.0, uBumpTexel.y)).r;
        float hD = texture(uBumpTex, vUv + vec2(0.0, uBumpTexel.y)).r;
        vec3 bumpNormal = normalize(vec3((hL - hR) * 2.6, (hU - hD) * 2.6, 1.0));
        return normalize(TBN * bumpNormal);
    }

    return N;
}

void main()
{
    vec3 albedo = uHasBase == 1 ? pow(texture(uBaseTex, vUv).rgb, vec3(2.2)) : vec3(0.72, 0.74, 0.78);
    float roughness = uHasRoughness == 1 ? texture(uRoughnessTex, vUv).r : 0.48;
    float metalness = uHasMetal == 1 ? texture(uMetalTex, vUv).r : 0.0;
    float ao = uHasAo == 1 ? texture(uAoTex, vUv).r : 1.0;
    float alpha = uHasAlpha == 1 ? texture(uAlphaTex, vUv).r : 1.0;

    // Channel-isolation views use a neutral matte surface so the shape/lighting
    // the map produces is the only thing being judged.
    if (uViewMode == 1 || uViewMode == 2) {
        albedo = vec3(0.72, 0.74, 0.78);
        roughness = 0.55;
        metalness = 0.0;
        ao = 1.0;
        alpha = 1.0;
    }

    roughness = clamp(roughness, 0.045, 0.98);
    metalness = clamp(metalness, 0.0, 1.0);
    ao = clamp(ao, 0.0, 1.0);

    vec3 N = sampleSurfaceNormal();
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 L = normalize(uLightDir);
    vec3 H = normalize(V + L);
    vec3 fillL = normalize(uFillDir);
    vec3 fillH = normalize(V + fillL);

    vec3 F0 = mix(vec3(0.04), albedo, metalness);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.0001);
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metalness);

    float NdotL = max(dot(N, L), 0.0);
    vec3 radiance = vec3(2.7, 2.55, 2.35);
    vec3 direct = (kD * albedo / 3.14159265 + specular) * radiance * NdotL;

    float fillNdotL = max(dot(N, fillL), 0.0);
    float fillSpec = pow(max(dot(N, fillH), 0.0), mix(20.0, 180.0, 1.0 - roughness));
    vec3 fill = (albedo * 0.12 * fillNdotL + F0 * fillSpec * 0.08);

    float sky = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambient = mix(vec3(0.03, 0.035, 0.045), vec3(0.18, 0.21, 0.25), sky) * albedo * (1.0 - metalness * 0.65);

    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    vec3 rim = mix(vec3(0.02, 0.03, 0.04), albedo, metalness * 0.7) * fresnel * 0.16;

    vec3 color = (direct + fill + ambient + rim) * ao;
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    fragColor = vec4(color, alpha);
}
)GLSL";
}

MaterialPreviewWidget::MaterialPreviewWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_placeholderText(QStringLiteral("3D Material Preview"))
    , m_vertexBuffer(QOpenGLBuffer::VertexBuffer)
    , m_indexBuffer(QOpenGLBuffer::IndexBuffer)
{
    setMinimumSize(220, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);

    connect(&m_frameTimer, &QTimer::timeout, this, [this]() {
        update();
    });
    m_frameTimer.start(16);
    m_elapsed.start();

    QFile file(glLogPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write("=== Material preview GL log ===\n");
        file.close();
    }

    QTimer::singleShot(1500, this, [this]() {
        logGl(QStringLiteral("Snapshot: glReady=%1 programOk=%2 programFailed=%3 images=%4 contextValid=%5 glLog=%6")
                  .arg(m_glReady)
                  .arg(m_program != nullptr)
                  .arg(m_programFailed)
                  .arg(m_images.size())
                  .arg(context() ? context()->isValid() : false)
                  .arg(glLogPath()));
    });
}

MaterialPreviewWidget::~MaterialPreviewWidget()
{
    if (context()) {
        makeCurrent();
        destroyTextures();
        m_vao.destroy();
        m_vertexBuffer.destroy();
        m_indexBuffer.destroy();
        delete m_program;
        m_program = nullptr;
        doneCurrent();
    }
}

void MaterialPreviewWidget::setMaps(const QMap<QString, QImage>& images)
{
    m_images = images;
    m_texturesDirty = true;
    update();
}

void MaterialPreviewWidget::clearMaps()
{
    m_images.clear();
    m_texturesDirty = true;
    update();
}

void MaterialPreviewWidget::setPlaceholderText(const QString& text)
{
    m_placeholderText = text;
    update();
}

void MaterialPreviewWidget::setPreviewMode(PreviewMode mode)
{
    if (m_previewMode == mode) {
        return;
    }
    m_previewMode = mode;
    update();
}

MaterialPreviewWidget::PreviewMode MaterialPreviewWidget::previewMode() const
{
    return m_previewMode;
}

void MaterialPreviewWidget::resetView()
{
    // A three-quarter view: the turntable spins around Y, but the cube also
    // needs a little vertical tilt and a yaw offset so two faces are visible
    // at once instead of looking at a single flat face head-on.
    m_pitch = 22.0f;
    m_manualYaw = 35.0f;
    m_distance = defaultDistance(m_shape);
    update();
}

void MaterialPreviewWidget::setPreviewShape(PreviewShape shape)
{
    if (m_shape == shape) {
        return;
    }

    m_shape = shape;
    m_distance = defaultDistance(shape);
    // The buffers hold the old shape's vertices, so they have to be rebuilt.
    // Doing it here (rather than lazily) means ensureGeometry() in paintGL sees
    // a clean slate and cannot draw last frame's geometry.
    if (context()) {
        makeCurrent();
        m_vao.destroy();
        m_vertexBuffer.destroy();
        m_indexBuffer.destroy();
        m_indexCount = 0;
        ensureGeometry();
        doneCurrent();
    }
    update();
}

MaterialPreviewWidget::PreviewShape MaterialPreviewWidget::previewShape() const
{
    return m_shape;
}

void MaterialPreviewWidget::initializeGL()
{
    logGl(QStringLiteral("initializeGL called"));
    initializeOpenGLFunctions();

    const QOpenGLContext* ctx = context();
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* glsl = glGetString(GL_SHADING_LANGUAGE_VERSION);
    logGl(QStringLiteral("GL version: %1")
              .arg(version ? QString::fromUtf8(reinterpret_cast<const char*>(version)) : QStringLiteral("<null>")));
    logGl(QStringLiteral("GL vendor: %1")
              .arg(vendor ? QString::fromUtf8(reinterpret_cast<const char*>(vendor)) : QStringLiteral("<null>")));
    logGl(QStringLiteral("GL renderer: %1")
              .arg(renderer ? QString::fromUtf8(reinterpret_cast<const char*>(renderer)) : QStringLiteral("<null>")));
    logGl(QStringLiteral("GLSL: %1")
              .arg(glsl ? QString::fromUtf8(reinterpret_cast<const char*>(glsl)) : QStringLiteral("<null>")));
    if (ctx) {
        const QSurfaceFormat format = ctx->format();
        logGl(QStringLiteral("Requested surface format: %1.%2 profile=%3")
                  .arg(format.majorVersion())
                  .arg(format.minorVersion())
                  .arg(QString::number(static_cast<int>(format.profile()))));
    }

    glEnable(GL_DEPTH_TEST);
    // Closed convex sphere: draw only the outward-facing surface. With an alpha
    // map blended in, culling the back hemisphere stops the far side showing
    // through transparent regions as dark, inward-facing geometry.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    checkGlError("initializeGL setup");

    ensureProgram();
    ensureGeometry();
    checkGlError("initializeGL geometry");

    if (!m_program) {
        logGl(QStringLiteral("Shader program is unavailable; preview will show placeholder."));
    }

    m_glReady = true;
    logGl(QStringLiteral("initializeGL finished (program %1)")
              .arg(m_program ? QStringLiteral("OK") : QStringLiteral("NULL")));
}

void MaterialPreviewWidget::resizeGL(int width, int height)
{
    glViewport(0, 0, width, height);
}

void MaterialPreviewWidget::paintEvent(QPaintEvent* event)
{
    // QPainter must not be used inside paintGL; all text fallbacks live here.
    // If the GL context never became valid (or the shader failed), draw the
    // placeholder on Qt's raster surface so the user sees readable text
    // instead of a black block.
    const bool contextReady = m_glReady && m_program && context() && context()->isValid();
    if (m_images.isEmpty() || !contextReady) {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(13, 18, 24));
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(QColor(145, 162, 205));
        painter.drawText(rect(), Qt::AlignCenter, m_placeholderText);
        return;
    }
    QOpenGLWidget::paintEvent(event);
}

void MaterialPreviewWidget::paintGL()
{
    glClearColor(0.06f, 0.08f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Defensive state asserts every frame: a closed convex sphere renders its
    // outward surface only, regardless of what any other widget or context swap
    // left in the state. Culling the back hemisphere keeps transparent alpha
    // regions from revealing the far side as inverted-lit inner geometry.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_DEPTH_TEST);

    if (m_images.isEmpty()) {
        return; // text placeholder is drawn in paintEvent (no QPainter in GL)
    }

    ensureProgram();
    ensureGeometry();
    if (m_texturesDirty) {
        rebuildTextures();
    }

    if (!m_program || !m_vertexBuffer.isCreated() || !m_indexBuffer.isCreated() || m_indexCount <= 0) {
        return;
    }

    if (!m_vao.isCreated()) {
        logGl(QStringLiteral("VAO missing before draw; re-creating geometry"));
        ensureGeometry();
    }

    const float now = static_cast<float>(m_elapsed.elapsed()) / 1000.0f;
    // Idle turntable: advances only while the user is NOT holding a drag, so
    // grabbing the ball pins it exactly and releasing lets it drift again.
    // The delta is clamped so a long stall (window hidden/minimized) does not
    // teleport the ball to a random angle.
    if (!m_dragging) {
        const float dt = std::min(now - m_lastSpinTime, 0.5f);
        m_spinOffset = wrapDegrees(m_spinOffset + dt * 14.0f, 0.0f, 360.0f);
    }
    m_lastSpinTime = now;
    const float autoYaw = wrapDegrees(m_manualYaw + m_spinOffset, 0.0f, 360.0f);

    QMatrix4x4 model;
    // Turntable order: yaw first around the world's vertical axis (always the
    // screen-vertical spin), then pitch in the yawed frame so a vertical drag
    // always tips the front of the ball toward/away from the camera instead of
    // rolling it sideways once yaw is non-zero.
    model.rotate(m_pitch, 1.0f, 0.0f, 0.0f);
    model.rotate(autoYaw, 0.0f, 1.0f, 0.0f);

    QMatrix4x4 view;
    const QVector3D cameraPos(0.0f, 0.0f, m_distance);
    view.lookAt(cameraPos, QVector3D(0.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f));

    QMatrix4x4 projection;
    projection.perspective(34.0f, static_cast<float>(width()) / static_cast<float>(std::max(1, height())), 0.1f, 20.0f);

    // Fixed studio key light (upper-left-front) + cool fill. The light stays
    // put so the material's look under rotation is constant and predictable.
    const QVector3D lightDir(0.52f, -0.38f, 0.82f);
    const QVector3D fillDir(-0.42f, 0.35f, 0.74f);
    const QImage bump = m_images.value(QStringLiteral("bump"));
    const QVector2D bumpTexel(bump.isNull() ? 1.0f / 1024.0f : 1.0f / std::max(1, bump.width()),
                              bump.isNull() ? 1.0f / 1024.0f : 1.0f / std::max(1, bump.height()));

    m_program->bind();
    checkGlError("program bind");
    m_program->setUniformValue("uMvp", projection * view * model);
    m_program->setUniformValue("uModel", model);
    m_program->setUniformValue("uNormalMatrix", model.normalMatrix());
    m_program->setUniformValue("uLightDir", lightDir.normalized());
    m_program->setUniformValue("uFillDir", fillDir.normalized());
    m_program->setUniformValue("uCameraPos", cameraPos);
    m_program->setUniformValue("uBumpTexel", bumpTexel);
    m_program->setUniformValue("uHasBase", m_baseTexture ? 1 : 0);
    m_program->setUniformValue("uHasBump", m_bumpTexture ? 1 : 0);
    m_program->setUniformValue("uHasNormal", m_normalTexture ? 1 : 0);
    m_program->setUniformValue("uHasRoughness", m_roughnessTexture ? 1 : 0);
    m_program->setUniformValue("uHasMetal", m_metalTexture ? 1 : 0);
    m_program->setUniformValue("uHasAo", m_aoTexture ? 1 : 0);
    m_program->setUniformValue("uHasAlpha", m_alphaTexture ? 1 : 0);
    m_program->setUniformValue("uViewMode", static_cast<int>(m_previewMode));

    bindTexture(0, m_baseTexture, "uBaseTex");
    bindTexture(1, m_bumpTexture, "uBumpTex");
    bindTexture(2, m_normalTexture, "uNormalTex");
    bindTexture(3, m_roughnessTexture, "uRoughnessTex");
    bindTexture(4, m_metalTexture, "uMetalTex");
    bindTexture(5, m_aoTexture, "uAoTex");
    bindTexture(6, m_alphaTexture, "uAlphaTex");
    checkGlError("texture binds");

    m_vao.bind();
    // GL_ARRAY_BUFFER is not part of VAO state: the attrib pointers below
    // reference whichever array buffer is bound right now.
    m_vertexBuffer.bind();
    m_indexBuffer.bind();
    const int stride = static_cast<int>(sizeof(PreviewVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(PreviewVertex, px)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(PreviewVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(PreviewVertex, u)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(PreviewVertex, tx)));
    checkGlError("attrib setup");

    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    checkGlError("drawElements");

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    m_vao.release();
    m_program->release();
}

void MaterialPreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
    }
    m_lastMousePos = event->pos();
    QOpenGLWidget::mousePressEvent(event);
}

void MaterialPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint delta = event->pos() - m_lastMousePos;
    if (event->buttons() & Qt::LeftButton) {
        m_dragging = true;
        // Keep both angles wrapped so the ball can be orbited freely through a
        // full 360 degrees instead of clamping up at the poles.
        m_manualYaw = wrapDegrees(m_manualYaw + static_cast<float>(delta.x()) * 0.45f, 0.0f, 360.0f);
        // Grab/trackball convention: the surface follows the cursor, so
        // dragging up tips the front of the ball up and swings the bottom into
        // view. Add the vertical delta to pitch it that way.
        m_pitch = wrapDegrees(m_pitch + static_cast<float>(delta.y()) * 0.35f, -180.0f, 180.0f);
        update();
    }
    m_lastMousePos = event->pos();
    QOpenGLWidget::mouseMoveEvent(event);
}

void MaterialPreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void MaterialPreviewWidget::wheelEvent(QWheelEvent* event)
{
    m_distance = std::clamp(m_distance - event->angleDelta().y() / 960.0f, 1.8f, 5.0f);
    update();
    event->accept();
}

void MaterialPreviewWidget::ensureProgram()
{
    if (m_program) {
        return;
    }

    auto* program = new QOpenGLShaderProgram(this);
    const bool vertexOk = program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    if (!vertexOk) {
        logGl(QStringLiteral("VERTEX shader compile failed: %1").arg(program->log()));
    }
    const bool fragmentOk = program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    if (!fragmentOk) {
        logGl(QStringLiteral("FRAGMENT shader compile failed: %1").arg(program->log()));
    }
    const bool linked = program->link();
    if (!linked) {
        logGl(QStringLiteral("Shader link failed: %1").arg(program->log()));
    }

    if (!vertexOk || !fragmentOk || !linked) {
        m_programFailed = true;
        logGl(QStringLiteral("Shader program is unavailable (compile vertex=%1 fragment=%2 link=%3).")
                  .arg(vertexOk)
                  .arg(fragmentOk)
                  .arg(linked));
        delete program;
        return;
    }

    m_program = program;
    logGl(QStringLiteral("Shader program linked OK"));
}

void MaterialPreviewWidget::ensureGeometry()
{
    if (m_vertexBuffer.isCreated() && m_indexBuffer.isCreated() && m_indexCount > 0
        && m_builtShape == m_shape) {
        return;
    }

    QVector<PreviewVertex> vertices;
    QVector<quint32> indices;
    if (m_shape == PreviewShape::Cube) {
        appendCube(vertices, indices);
    } else {
        appendSphere(vertices, indices);
    }

    if (!m_vao.isCreated()) {
        m_vao.create();
    }
    if (!m_vertexBuffer.isCreated()) {
        m_vertexBuffer.create();
    }
    if (!m_indexBuffer.isCreated()) {
        m_indexBuffer.create();
    }

    // Core-profile contexts require a bound VAO for vertex attribute state.
    m_vao.bind();
    m_vertexBuffer.bind();
    m_vertexBuffer.allocate(vertices.constData(), static_cast<int>(vertices.size() * sizeof(PreviewVertex)));
    m_vertexBuffer.release();

    m_indexBuffer.bind();
    m_indexBuffer.allocate(indices.constData(), static_cast<int>(indices.size() * sizeof(quint32)));
    m_indexBuffer.release();
    m_vao.release();

    m_indexCount = indices.size();
    m_builtShape = m_shape;
}

void MaterialPreviewWidget::rebuildTextures()
{
    destroyTextures();
    m_baseTexture = makeTexture(m_images.value(QStringLiteral("base")));
    m_bumpTexture = makeTexture(m_images.value(QStringLiteral("bump")));
    m_normalTexture = makeTexture(m_images.value(QStringLiteral("normal")));
    m_roughnessTexture = makeTexture(m_images.value(QStringLiteral("roughness")));
    m_metalTexture = makeTexture(m_images.value(QStringLiteral("metal")));
    m_aoTexture = makeTexture(m_images.value(QStringLiteral("ao")));
    m_alphaTexture = makeTexture(m_images.value(QStringLiteral("alpha")));
    m_texturesDirty = false;
}

void MaterialPreviewWidget::destroyTextures()
{
    delete m_baseTexture;
    delete m_bumpTexture;
    delete m_normalTexture;
    delete m_roughnessTexture;
    delete m_metalTexture;
    delete m_aoTexture;
    delete m_alphaTexture;
    m_baseTexture = nullptr;
    m_bumpTexture = nullptr;
    m_normalTexture = nullptr;
    m_roughnessTexture = nullptr;
    m_metalTexture = nullptr;
    m_aoTexture = nullptr;
    m_alphaTexture = nullptr;
}

void MaterialPreviewWidget::checkGlError(const char* tag)
{
    GLenum error = glGetError();
    while (error != GL_NO_ERROR) {
        logGl(QStringLiteral("GL error 0x%1 at %2").arg(QString::number(error, 16)).arg(QString::fromUtf8(tag)));
        error = glGetError();
    }
}

void MaterialPreviewWidget::bindTexture(int unit, QOpenGLTexture* texture, const char* uniformName)
{
    if (!m_program) {
        return;
    }

    m_program->setUniformValue(uniformName, unit);
    if (!texture) {
        return;
    }

    glActiveTexture(GL_TEXTURE0 + unit);
    texture->bind();
}
