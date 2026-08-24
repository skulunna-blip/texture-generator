#pragma once

#include <QImage>
#include <QMap>
#include <QSize>
#include <QString>

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
    static QImage createHeightMap(const QImage& image, double strength, TextureBackendMode backendMode, bool wrap);
    static QImage createNormalMap(const QImage& heightMap, double strength, TextureBackendMode backendMode, bool wrap);
    static QImage createRoughnessMap(const QImage& image, const QImage& heightMap, TextureBackendMode backendMode, bool wrap);
    static QImage createMetalMap(const QImage& image, const QImage& roughnessMap, TextureBackendMode backendMode);
    static QImage createAoMap(const QImage& image, const QImage& heightMap, TextureBackendMode backendMode, bool wrap);
    static QImage createAlphaMap(const QImage& image);
    static double luminance(const QRgb& color);
};
