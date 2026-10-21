#pragma once

#include "TextureGenerator.h"

#include <QImage>
#include <QMap>
#include <QString>

#include <functional>

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

    // 0-100 across all maps, plus the name of the map being worked on. The
    // engine calls it from the worker thread, so a UI receiver has to marshal
    // to its own thread (QMetaObject::invokeMethod with Qt::QueuedConnection).
    using ProgressCallback = std::function<void(int percent, const QString& label)>;

    // Upscales every map in sourceFiles into <outputRoot>/upscaled_<scale>x/
    // and returns the new maps. Blocking call; run it in a worker thread from
    // the UI.
    static Result upscale(const QMap<QString, QString>& sourceFiles,
                          const QString& outputRoot,
                          int scale,
                          StyleMode style,
                          ProgressCallback progress = {});

    // One image, which is what the app uses: the source the user dropped in is
    // enlarged once, and every map is then generated from that. Scaling the
    // maps afterwards would invent detail in AO and roughness, where invented
    // detail is the last thing you want, and it would redo the same inference
    // six times.
    static bool upscaleImage(const QString& sourcePath,
                             const QString& outputPath,
                             int scale,
                             StyleMode style,
                             const ProgressCallback& progress = {},
                             QString* error = nullptr);
};
