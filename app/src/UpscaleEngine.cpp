#include "UpscaleEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <algorithm>

namespace
{
constexpr int kProcessTimeoutMs = 240000;

// The tool prints one "NN.NN%" line per tile (a 2K map is about sixty of them).
// Anything else it prints - the Vulkan device banner, errors - is ignored here;
// it is still captured and reported if the process fails.
int parsePercentLine(const QByteArray& line)
{
    const QByteArray trimmed = line.trimmed();
    if (!trimmed.endsWith('%')) {
        return -1;
    }
    bool ok = false;
    const double value = trimmed.left(trimmed.size() - 1).toDouble(&ok);
    if (!ok) {
        return -1;
    }
    return std::clamp(static_cast<int>(value), 0, 100);
}

QString findBinaryUnder(const QString& root)
{
    const QStringList relative = {
        QStringLiteral("realesrgan-ncnn-vulkan/realesrgan-ncnn-vulkan.exe"),
        QStringLiteral("realesrgan-ncnn-vulkan-20220424-windows/realesrgan-ncnn-vulkan.exe"),
    };
    for (const QString& candidate : relative) {
        const QString path = QDir(root).filePath(candidate);
        if (QFile::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }
    return QString();
}

// The bundled builds of realesrgan-ncnn-vulkan cannot do 2x with the x4 models:
// -s 2 writes the top-left quarter of the 4x result instead of a resampled
// full frame. Verified two ways - a 64 px test image split into four coloured
// quadrants comes back entirely red, with and without -t, while -s 3 and -s 4
// keep all four quadrants. So the model always runs at its native 4x and any
// other scale is resampled here; supersampling down from 4x is also the better
// way to get 2x than asking the network for it.
constexpr int kNativeScale = 4;

bool resampleDown(QImage& image, int scale)
{
    if (scale == kNativeScale) {
        return true;
    }
    const int width = image.width() * scale / kNativeScale;
    const int height = image.height() * scale / kNativeScale;
    if (width < 1 || height < 1) {
        return false;
    }
    image = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return true;
}

bool runRealesrgan(const QString& binary,
                   const QString& modelDir,
                   const QString& modelName,
                   const QString& input,
                   const QString& output,
                   int scale,
                   const std::function<void(int)>& onPercent)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(binary, {
        QStringLiteral("-i"), input,
        QStringLiteral("-o"), output,
        QStringLiteral("-n"), modelName,
        QStringLiteral("-m"), modelDir,
        QStringLiteral("-s"), QString::number(scale),
        QStringLiteral("-f"), QStringLiteral("png"),
        QStringLiteral("-t"), QStringLiteral("256"),
    });
    if (!process.waitForStarted(15000)) {
        qWarning("realesrgan: could not start (%s)", qPrintable(binary));
        return false;
    }

    // Read while it runs rather than waiting for the exit. A 2K map takes
    // minutes and the tool reports its own tiling progress the whole way; with
    // waitForFinished all of that arrived at once at the end, which is exactly
    // the silence a progress bar is meant to replace.
    QByteArray raw;      // everything printed, for the failure message
    QByteArray partial;  // the line being assembled
    QElapsedTimer elapsed;
    elapsed.start();
    int lastPercent = -1;
    auto pump = [&]() {
        const QByteArray chunk = process.readAll();
        raw += chunk;
        partial += chunk;
        int lineStart = 0;
        for (int i = 0; i < partial.size(); ++i) {
            if (partial.at(i) != '\n' && partial.at(i) != '\r') {
                continue;
            }
            const int percent = parsePercentLine(partial.mid(lineStart, i - lineStart));
            if (percent >= 0 && percent != lastPercent) {
                lastPercent = percent;
                if (onPercent) {
                    onPercent(percent);
                }
            }
            lineStart = i + 1;
        }
        partial.remove(0, lineStart);
    };

    bool timedOut = false;
    while (process.state() != QProcess::NotRunning) {
        if (elapsed.elapsed() > kProcessTimeoutMs) {
            timedOut = true;
            break;
        }
        process.waitForReadyRead(200);
        pump();
    }
    pump();
    if (timedOut) {
        process.kill();
        process.waitForFinished(2000);
        qWarning("realesrgan: timed out");
        return false;
    }
    // The loop above only leaves without timing out once the process state is
    // NotRunning, so there is nothing left to wait for here - and calling
    // waitForFinished() on an already-finished QProcess returns false, which is
    // exactly the kind of "already done" that looks like a failure.

    const int exitCode = process.exitCode();
    if (process.exitStatus() != QProcess::NormalExit || exitCode != 0) {
        qWarning("realesrgan: exit %d: %s", exitCode, raw.constData());
        return false;
    }
    if (onPercent) {
        onPercent(100);
    }
    return QFile::exists(output);
}

// Direct high-quality resize for the normal map (see UpscaleEngine.h).
bool resizeDirectly(const QString& input, const QString& output, int scale)
{
    QImage image(input);
    if (image.isNull()) {
        return false;
    }
    const QImage scaled = image.scaled(image.width() * scale,
                                       image.height() * scale,
                                       Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    return scaled.save(output, "PNG");
}
} // namespace

QString UpscaleEngine::findBinary()
{
    // 1. explicit override (power users, tests)
    const QString overridePath = qEnvironmentVariable("TG_REALESRGAN_BIN");
    if (!overridePath.isEmpty() && QFile::exists(overridePath)) {
        return QFileInfo(overridePath).absoluteFilePath();
    }

    // 2. beside the running executable (deployed layout)
    QString found = findBinaryUnder(QCoreApplication::applicationDirPath());
    if (!found.isEmpty()) {
        return found;
    }

    // 3. project tools/ folder, walking up from the exe (dev layout:
    //    TextureGeneratorQt.exe lives in build/ under the project root)
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 6 && !dir.isRoot(); ++depth) {
        found = findBinaryUnder(QDir(dir.path()).filePath(QStringLiteral("tools")));
        if (!found.isEmpty()) {
            return found;
        }
        dir.cdUp();
    }

    // 4. current working directory (scripts, tests run from the project root)
    return findBinaryUnder(QDir::currentPath());
}

bool UpscaleEngine::isAvailable()
{
    return !findBinary().isEmpty();
}

bool UpscaleEngine::upscaleImage(const QString& sourcePath,
                                 const QString& outputPath,
                                 int scale,
                                 StyleMode style,
                                 const ProgressCallback& progress,
                                 QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (scale < 1) {
        scale = 4;
    }
    const QString binary = findBinary();
    if (binary.isEmpty()) {
        return fail(QStringLiteral("未找到 Real-ESRGAN。请把 realesrgan-ncnn-vulkan 放到 tools/ 目录（见 _get_realesrgan.ps1）。"));
    }
    const QString modelDir = QDir(QFileInfo(binary).absolutePath()).filePath(QStringLiteral("models"));
    const QString modelName = (style == StyleMode::Anime)
        ? QStringLiteral("realesrgan-x4plus-anime")
        : QStringLiteral("realesrgan-x4plus");
    const QString label = QFileInfo(sourcePath).fileName();

    if (progress) {
        progress(0, label);
    }
    const bool ok = runRealesrgan(binary, modelDir, modelName, sourcePath, outputPath, kNativeScale,
                                  [&](int percent) {
                                      if (progress) {
                                          progress(percent, label);
                                      }
                                  });
    if (!ok) {
        return fail(QStringLiteral("放大 %1 失败。").arg(label));
    }
    if (scale != kNativeScale) {
        QImage produced(outputPath);
        if (produced.isNull() || !resampleDown(produced, scale) || !produced.save(outputPath, "PNG")) {
            return fail(QStringLiteral("放大 %1 失败。").arg(label));
        }
    }
    if (progress) {
        progress(100, label);
    }
    return true;
}

UpscaleEngine::Result UpscaleEngine::upscale(const QMap<QString, QString>& sourceFiles,
                                             const QString& outputRoot,
                                             int scale,
                                             StyleMode style,
                                             ProgressCallback progress)
{
    Result result;
    if (scale < 1) {
        scale = 4;
    }

    const QString binary = findBinary();
    if (binary.isEmpty()) {
        result.error = QStringLiteral("未找到 Real-ESRGAN。请把 realesrgan-ncnn-vulkan 放到 tools/ 目录（见 _get_realesrgan.ps1）。");
        return result;
    }

    const QString outDir = QDir(outputRoot).filePath(QStringLiteral("upscaled_%1x").arg(scale));
    if (!QDir().mkpath(outDir)) {
        result.error = QStringLiteral("无法创建输出目录：%1").arg(outDir);
        return result;
    }

    // Progress is reported across all maps, not per map: one 2K map is minutes
    // of work, so "3 of 6" alone would still look frozen for long stretches.
    // The inference percentage the tool prints is folded into the map's share.
    const int total = std::max(1, static_cast<int>(sourceFiles.size()));
    int done = 0;
    const auto report = [&](int mapPercent, const QString& label) {
        if (!progress) {
            return;
        }
        const int overall = (done * 100 + std::clamp(mapPercent, 0, 100)) / total;
        progress(std::clamp(overall, 0, 100), label);
    };

    for (auto it = sourceFiles.cbegin(); it != sourceFiles.cend(); ++it) {
        const QString key = it.key();
        const QString source = it.value();
        if (!QFile::exists(source)) {
            ++done;
            continue;
        }
        const QString outPath = QDir(outDir).filePath(QFileInfo(source).fileName());
        const QString label = QFileInfo(source).fileName();
        report(0, label);

        bool ok = false;
        if (key == QStringLiteral("normal")) {
            // Vector data: never handed to the network. Interpolation is the
            // only resize that keeps a normal map's encoded direction intact.
            ok = resizeDirectly(source, outPath, scale);
        } else {
            ok = upscaleImage(source, outPath, scale, style,
                              [&](int percent, const QString&) { report(percent, label); });
        }
        if (!ok) {
            result.error = QStringLiteral("放大 %1 失败。").arg(QFileInfo(source).fileName());
            return result;
        }
        ++done;
        report(100, label);

        QImage image(outPath);
        if (!image.isNull()) {
            result.images.insert(key, image);
            result.filePaths.insert(key, outPath);
        }
    }

    result.outputDirectory = outDir;
    result.ok = !result.images.isEmpty();
    if (!result.ok && result.error.isEmpty()) {
        result.error = QStringLiteral("放大结果无法读取。");
    }
    return result;
}
