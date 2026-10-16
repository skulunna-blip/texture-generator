#pragma once

#include <QImage>
#include <QMap>
#include <QSize>
#include <QString>
#include <QVector>

enum class TextureBackendMode
{
    ClassicFast,
    EnhancedDetail,
    AiBridge
};

// Stylization applied to the base color map (and roughness). Realistic keeps the
// input as-is; Anime flattens tones into posterized luma bands, keeps hue, adds
// Sobel edge outlines and boosts saturation for a cell-shaded / anime look.
enum class StyleMode
{
    Realistic,
    Anime
};

struct TextureOptions
{
    int resolution = 1024;
    bool square = true;
    bool seamless = false;
    double bumpStrength = 1.0;
    double normalStrength = 1.0;
    TextureBackendMode backendMode = TextureBackendMode::EnhancedDetail;
    StyleMode styleMode = StyleMode::Realistic;
    // Flattens the lighting a photo arrived with, so the base color is closer to
    // true albedo. Independent of the backend choice - it is a classical pass,
    // but it borrows the depth map as a guide when the neural backend is on.
    bool removeLighting = false;
    double delightStrength = 0.8;
    // Whether the live preview also runs the depth model. Off by default: the
    // final generate is what has to be right, and the classic preview stays
    // instant. The depth map itself is cached, so once the model has run for a
    // source, every later slider drag is free either way.
    bool aiPreview = false;
    bool generateBaseColor = true;
    bool generateBump = true;
    bool generateNormal = true;
    bool generateRoughness = true;
    bool generateMetal = true;
    bool generateAo = true;
    bool generateAlpha = true;
};

struct TextureResult
{
    QString jobId;
    QString outputDirectory;
    QSize size;
    QMap<QString, QImage> images;
    QMap<QString, QString> filePaths;
};

// A single-channel float image.
//
// The height pipeline works in float rather than in an 8-bit grey image because
// every stage of it is a difference of two blurs of the same image. Rounding
// each blur into 8 bits first puts an error into that difference that is the
// same size as the relief the difference is measuring - measured on a smooth
// surface, the shipped normal map's entire tilt was quantisation. The 8-bit
// image still exists, but only as the artefact bump.png is written from.
struct HeightField
{
    int width = 0;
    int height = 0;
    QVector<float> values;

    void reset(int w, int h)
    {
        width = w;
        height = h;
        values.resize(static_cast<qsizetype>(w) * h);
    }

    float at(int x, int y) const { return values[static_cast<qsizetype>(y) * width + x]; }
    float& at(int x, int y) { return values[static_cast<qsizetype>(y) * width + x]; }
};

class TextureGenerator
{
public:
    static TextureResult generate(const QImage& source, const TextureOptions& options, const QString& baseOutputDirectory);
    static TextureResult preview(const QImage& source, const TextureOptions& options);
    static bool isBackendAvailable(TextureBackendMode backendMode);

private:
    static TextureResult buildResult(const QImage& source, const TextureOptions& options, const QString& baseOutputDirectory, bool writeFiles);
    static QImage prepareSource(const QImage& source, const TextureOptions& options);
    static QImage makeSeamless(const QImage& image);
    static QImage applyAnimeStyle(const QImage& image);
    static HeightField createHeightField(const QImage& image, double strength, TextureBackendMode backendMode, bool wrap);
    // Adds the relief the depth model sees on top of a luminance height map.
    // Depth supplies the macro geometry (which object is in front of which) and
    // luminance supplies the micro detail; neither alone is enough. A flat depth
    // map contributes nothing, so the classic result passes through untouched.
    static HeightField mergeDepthRelief(const HeightField& heightMap, const QImage& depth, double strength, bool wrap);
    static QImage createNormalMap(const HeightField& heightMap, double strength, TextureBackendMode backendMode, bool wrap);
    static QImage createRoughnessMap(const QImage& image, const HeightField& heightMap, TextureBackendMode backendMode, bool wrap);
    static QImage createMetalMap(const QImage& image, const QImage& roughnessMap, TextureBackendMode backendMode);
    static QImage createAoMap(const QImage& image, const HeightField& heightMap, TextureBackendMode backendMode, bool wrap);
    static QImage createAlphaMap(const QImage& image);
    static double luminance(const QRgb& color);
};
