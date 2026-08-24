#include "UpscaleEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace
{
constexpr int kProcessTimeoutMs = 240000;

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

bool runRealesrgan(const QString& binary,
                   const QString& modelDir,
                   const QString& modelName,
                   const QString& input,
                   const QString& output,
                   int scale)
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
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        qWarning("realesrgan: timed out");
        return false;
    }
    const int exitCode = process.exitCode();
    const QByteArray log = process.readAll();
    if (process.exitStatus() != QProcess::NormalExit || exitCode != 0) {
        qWarning("realesrgan: exit %d: %s", exitCode, log.constData());
        return false;
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

UpscaleEngine::Result UpscaleEngine::upscale(const QMap<QString, QString>& sourceFiles,
                                             const QString& outputRoot,
                                             int scale,
                                             StyleMode style)
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

    const QString exeDir = QFileInfo(binary).absolutePath();
    const QString modelDir = QDir(exeDir).filePath(QStringLiteral("models"));
    const QString modelName = (style == StyleMode::Anime)
        ? QStringLiteral("realesrgan-x4plus-anime")
        : QStringLiteral("realesrgan-x4plus");

    const QString outDir = QDir(outputRoot).filePath(QStringLiteral("upscaled_%1x").arg(scale));
    if (!QDir().mkpath(outDir)) {
        result.error = QStringLiteral("无法创建输出目录：%1").arg(outDir);
        return result;
    }

    for (auto it = sourceFiles.cbegin(); it != sourceFiles.cend(); ++it) {
        const QString key = it.key();
        const QString source = it.value();
        if (!QFile::exists(source)) {
            continue;
        }
        const QString outPath = QDir(outDir).filePath(QFileInfo(source).fileName());

        const bool ok = (key == QStringLiteral("normal"))
            ? resizeDirectly(source, outPath, scale)
            : runRealesrgan(binary, modelDir, modelName, source, outPath, scale);
        if (!ok) {
            result.error = QStringLiteral("放大 %1 失败。").arg(QFileInfo(source).fileName());
            return result;
        }

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
