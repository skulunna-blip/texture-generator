#pragma once

#include "TextureGenerator.h"

#include <QImage>
#include <QMap>
#include <QString>

// Super-resolution for the generated maps using the bundled Real-ESRGAN
// (realesrgan-ncnn-vulkan). The same model family covers both style modes:
// realesrgan-x4plus for realistic/photographic maps, realesrgan-x4plus-anime
// for anime/cel-shaded maps. The normal map is resampled directly instead of
// going through the AI model, because super-resolution would hallucinate
// tangent-space directions and break the map's unit-vector semantics.
class UpscaleEngine
{
public:
    struct Result
    {
        bool ok = false;
        QString error;
        QMap<QString, QImage> images;
        QMap<QString, QString> filePaths;
        QString outputDirectory;
    };

    // Locates realesrgan-ncnn-vulkan.exe: first from the TG_REALESRGAN_BIN
    // environment variable, then next to the app, then under the project
    // tools/ folder (walking up from the exe). Returns an empty string when
    // not bundled.
    static QString findBinary();
    static bool isAvailable();

    // Upscales every map in sourceFiles into <outputRoot>/upscaled_<scale>x/
    // and returns the new maps. Blocking call; run it in a worker thread from
    // the UI.
    static Result upscale(const QMap<QString, QString>& sourceFiles,
                          const QString& outputRoot,
                          int scale,
                          StyleMode style);
};
