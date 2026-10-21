#include "TextureGenerator.h"

#include "PatchSynthesis.h"

#include "DepthEstimator.h"
#include "LightingRemoval.h"
#include "SeamCut.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QMutex>

#include <cstdio>
#include <stdexcept>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace
{
// Set TG_PROFILE=1 to have buildResult print where its time went. The preview
// is interactive, so "it feels slow" has to become a stage breakdown before
// anything is worth changing; tg_preview_timing drives the configurations and
// this says which stage inside them is the cost.
struct Profile
{
    QElapsedTimer timer;
    bool enabled = qEnvironmentVariableIsSet("TG_PROFILE");

    Profile()
    {
        if (enabled) {
            timer.start();
        }
    }

    void step(const char* label)
    {
        if (!enabled) {
            return;
        }
        std::printf("PROFILE %-16s %5lld ms\n", label, static_cast<long long>(timer.restart()));
        std::fflush(stdout);
    }
};

QImage cropToSquare(const QImage& source)
{
    const int side = std::min(source.width(), source.height());
    const QRect rect((source.width() - side) / 2, (source.height() - side) / 2, side, side);
    return source.copy(rect);
}

// Square by reflection: the short side is grown by mirroring the picture back
// and forth. Nothing is distorted and nothing is invented, which is exactly
// what a texture with a repeating pattern wants; on a photograph the mirror
// axes are visible as symmetry.
QImage mirrorToSquare(const QImage& source)
{
    const int side = std::max(source.width(), source.height());
    QImage out(side, side, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const int w = source.width();
    const int h = source.height();
    const QImage src = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < side; ++y) {
        // Reflect every `h` rows; an odd period needs the fold, an even one a
        // plain modulo. The fold is what keeps out(0) == out(side-1).
        const int sy = h <= 1 ? 0 : (y < h ? y : (y / h) % 2 == 0 ? y % h : h - 1 - (y % h));
        const QRgb* in = reinterpret_cast<const QRgb*>(src.constScanLine(sy));
        QRgb* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < side; ++x) {
            const int sx = w <= 1 ? 0 : (x < w ? x : (x / w) % 2 == 0 ? x % w : w - 1 - (x % w));
            line[x] = in[sx];
        }
    }
    return out;
}

int wrapIndex(int index, int mod)
{
    int v = index % mod;
    return v < 0 ? v + mod : v;
}

// Separable box blur of a 0..1 mask, wrapping at the edges. Used to soften the
// seam cut per pyramid level.
std::vector<float> blurMask(const std::vector<float>& mask, int width, int height, int radius)
{
    const float inverseWindow = 1.0f / static_cast<float>(2 * radius + 1);
    std::vector<float> horizontal(mask.size(), 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                sum += mask[static_cast<size_t>(y) * width + wrapIndex(x + k, width)];
            }
            horizontal[static_cast<size_t>(y) * width + x] = sum * inverseWindow;
        }
    }

    std::vector<float> out(mask.size(), 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                sum += horizontal[static_cast<size_t>(wrapIndex(y + k, height)) * width + x];
            }
            out[static_cast<size_t>(y) * width + x] = sum * inverseWindow;
        }
    }
    return out;
}

QImage boxBlurGray(const QImage& image, int radius, bool wrap)
{
    QImage src = image.convertToFormat(QImage::Format_Grayscale8);
    if (radius <= 0 || src.isNull()) {
        return src;
    }

    const int width = src.width();
    const int height = src.height();
    const int pad = wrap ? radius : 0;
    const int pw = width + 2 * pad;
    const int ph = height + 2 * pad;

    // In seamless mode the blur must wrap around the edges so derived maps stay
    // tileable; otherwise clamping re-introduces the seam at the boundary.
    QImage padded;
    if (wrap) {
        padded = QImage(pw, ph, QImage::Format_Grayscale8);
        for (int y = 0; y < ph; ++y) {
            const int sy = wrapIndex(y - pad, height);
            const uchar* srcLine = src.constScanLine(sy);
            uchar* outLine = padded.scanLine(y);
            for (int x = 0; x < pw; ++x) {
                outLine[x] = srcLine[wrapIndex(x - pad, width)];
            }
        }
    } else {
        padded = src; // implicit share, read-only
    }

    QImage blurred(pw, ph, QImage::Format_Grayscale8);

    const int stride = pw + 1;
    QVector<int> integral((pw + 1) * (ph + 1), 0);

    for (int y = 0; y < ph; ++y) {
        const uchar* srcLine = padded.constScanLine(y);
        int rowSum = 0;
        const int currentRow = (y + 1) * stride;
        const int previousRow = y * stride;
        for (int x = 0; x < pw; ++x) {
            rowSum += srcLine[x];
            integral[currentRow + x + 1] = integral[previousRow + x + 1] + rowSum;
        }
    }

    for (int y = 0; y < ph; ++y) {
        uchar* outLine = blurred.scanLine(y);
        const int y0 = std::max(0, y - radius);
        const int y1 = std::min(ph - 1, y + radius);
        const int topRow = y0 * stride;
        const int bottomRow = (y1 + 1) * stride;

        for (int x = 0; x < pw; ++x) {
            const int x0 = std::max(0, x - radius);
            const int x1 = std::min(pw - 1, x + radius);
            const int area = (x1 - x0 + 1) * (y1 - y0 + 1);
            const int sum = integral[bottomRow + x1 + 1] - integral[topRow + x1 + 1] - integral[bottomRow + x0] + integral[topRow + x0];
            outLine[x] = static_cast<uchar>(sum / std::max(1, area));
        }
    }

    if (!wrap) {
        return blurred;
    }

    QImage dst(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        const uchar* blLine = blurred.constScanLine(pad + y);
        memcpy(dst.scanLine(y), blLine + pad, static_cast<size_t>(width));
    }
    return dst;
}

// The same box blur as boxBlurGray, and deliberately the same shape, but the
// result stays in float instead of being rounded into a grey image.
//
// The height pipeline is a chain of differences of blurs of one image, so every
// rounding is subtracted from another rounding rather than from the signal. On a
// smooth surface the four intermediate blurs being 8-bit accounted for the whole
// of the normal map's detail: the shipped normal tilted 0.26 deg on average and
// measured 0.27 deg away from the smooth field it was supposed to describe,
// while rounding only the final output instead recovered none of it. Keeping the
// field in float recovers all of it.
HeightField boxBlurField(const HeightField& src, int radius, bool wrap)
{
    if (radius <= 0 || src.width <= 0 || src.height <= 0) {
        return src;
    }

    const int width = src.width;
    const int height = src.height;
    const int pad = wrap ? radius : 0;
    const int pw = width + 2 * pad;
    const int ph = height + 2 * pad;

    // In seamless mode the blur wraps so derived maps stay tileable; otherwise
    // there is no padding and the window clamps at the border instead.
    QVector<double> padded(static_cast<qsizetype>(pw) * ph, 0.0);
    for (int y = 0; y < ph; ++y) {
        const int sy = wrap ? wrapIndex(y - pad, height) : y;
        for (int x = 0; x < pw; ++x) {
            const int sx = wrap ? wrapIndex(x - pad, width) : x;
            padded[static_cast<qsizetype>(y) * pw + x] = src.at(sx, sy);
        }
    }

    const int stride = pw + 1;
    QVector<double> integral(static_cast<qsizetype>(stride) * (ph + 1), 0.0);
    for (int y = 0; y < ph; ++y) {
        double rowSum = 0.0;
        const qsizetype currentRow = static_cast<qsizetype>(y + 1) * stride;
        const qsizetype previousRow = static_cast<qsizetype>(y) * stride;
        for (int x = 0; x < pw; ++x) {
            rowSum += padded[static_cast<qsizetype>(y) * pw + x];
            integral[currentRow + x + 1] = integral[previousRow + x + 1] + rowSum;
        }
    }

    HeightField out;
    out.reset(width, height);
    for (int y = 0; y < height; ++y) {
        const int py = y + pad;
        const int y0 = std::max(0, py - radius);
        const int y1 = std::min(ph - 1, py + radius);
        const qsizetype topRow = static_cast<qsizetype>(y0) * stride;
        const qsizetype bottomRow = static_cast<qsizetype>(y1 + 1) * stride;
        for (int x = 0; x < width; ++x) {
            const int px = x + pad;
            const int x0 = std::max(0, px - radius);
            const int x1 = std::min(pw - 1, px + radius);
            const int area = (x1 - x0 + 1) * (y1 - y0 + 1);
            const double sum = integral[bottomRow + x1 + 1] - integral[topRow + x1 + 1]
                             - integral[bottomRow + x0] + integral[topRow + x0];
            out.at(x, y) = static_cast<float>(sum / std::max(1, area));
        }
    }
    return out;
}

// Auto-levels for a height field: the 1st..99th percentile of the height is
// stretched to the full range, the way a height-to-normal tool does it. A
// normal map derived from the raw field inherits the photo's contrast, so the
// same slider did wildly different things on a flat wall and on a brick; the
// stretch removes that dependence. It is capped, because a source with no
// relief at all has a height range made of sensor noise and auto-levels would
// promote that noise to the whole range.
struct HeightLevels
{
    double low = 0.0;
    double stretch = 1.0;
    // Median of the levelled field: the height most of the surface sits at.
    // Only the bump export uses it (the normal map needs the stretch alone),
    // and only above gain 1, where expanding around mid-grey would push a
    // surface that does not sit at mid-grey - a tiled floor photographed from
    // above, for instance - into the top of the range and clip it there.
    double median = 0.5;
};
HeightLevels heightLevels(const HeightField& field);

// The 8-bit image is an export format, not part of the computation. This is the
// only place the height field is allowed to lose precision. Defined below
// scaledRadius so it can use saturate().
QImage heightFieldToImage(const HeightField& field, const HeightLevels& levels, double gain, int bits);

QString timeStampedJobId()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

bool anyMapEnabled(const TextureOptions& options)
{
    return options.generateBaseColor
        || options.generateBump
        || options.generateNormal
        || options.generateRoughness
        || options.generateMetal
        || options.generateAo
        || options.generateAlpha;
}

void storeImage(TextureResult& result,
                const QString& key,
                const QString& fileName,
                const QImage& image,
                bool writeFiles)
{
    result.images.insert(key, image);
    if (writeFiles) {
        const QString path = QDir(result.outputDirectory).filePath(fileName);
        image.save(path);
        result.filePaths.insert(key, path);
    }
}

double normalizedSample(const QImage& image, int x, int y)
{
    const int sx = std::clamp(x, 0, image.width() - 1);
    const int sy = std::clamp(y, 0, image.height() - 1);
    return image.constScanLine(sy)[sx] / 255.0;
}

double saturate(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

// Scale a blur radius relative to the image size so the same relative feature
// gets the same treatment at 1024, 2048 and 4096 px. Fixed radii would make
// high-resolution outputs progressively smoother than the preview.
int scaledRadius(int baseRadius, const QImage& image)
{
    const int side = std::max(image.width(), image.height());
    const double scale = static_cast<double>(side) / 1024.0;
    return std::max(1, static_cast<int>(std::round(baseRadius * scale)));
}

// The 8-bit image is an export format, not part of the computation. This is the
// only place the height field is allowed to lose precision.
HeightLevels heightLevels(const HeightField& field)
{
    constexpr double kMaxStretch = 8.0;
    constexpr int kBins = 1024;
    HeightLevels levels;
    if (field.width <= 0 || field.height <= 0) {
        return levels;
    }

    std::vector<long long> histogram(static_cast<size_t>(kBins), 0);
    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            const double v = std::clamp(static_cast<double>(field.at(x, y)), 0.0, 1.0);
            const int bin = std::clamp(static_cast<int>(v * (kBins - 1) + 0.5), 0, kBins - 1);
            ++histogram[static_cast<size_t>(bin)];
        }
    }
    auto percentile = [&](double p) {
        const long long wanted = static_cast<long long>(p * field.width * field.height);
        long long running = 0;
        for (int b = 0; b < kBins; ++b) {
            running += histogram[static_cast<size_t>(b)];
            if (running >= wanted) {
                return static_cast<double>(b) / (kBins - 1);
            }
        }
        return 1.0;
    };

    levels.low = percentile(0.01);
    const double high = percentile(0.99);
    const double range = high - levels.low;
    levels.stretch = range > 1e-6 ? std::min(1.0 / range, kMaxStretch) : kMaxStretch;
    levels.median = (percentile(0.5) - levels.low) * levels.stretch;
    return levels;
}

QImage heightFieldToImage(const HeightField& field, const HeightLevels& levels, double gain, int bits)
{
    // The same two steps the normal map takes: auto-levels first, then the
    // relief slider as a plain contrast gain. Exporting the raw field instead
    // meant the bump map kept the source's contrast while the normal map did
    // not - the two disagreed about the same surface, and moving the slider
    // changed one and (after the leveling) not the other.
    const double clampedGain = std::clamp(gain, 0.0, 3.0);

    // Up to gain 1 this is the plain stretch it always was, and the exported
    // bytes are unchanged.
    //
    // Above 1 it is not: the plain stretch pushes everything outside the
    // middle 1/gain band past the ends, and saturate() turns those regions
    // into one value. On a tiled photograph that was 27% of the map flat white
    // at gain 2 and 70% at gain 3, which reads as the relief having
    // disappeared. The extra contrast is spent on the middle instead, with a
    // slope-matched shoulder that rolls the tails off and keeps the map
    // monotone, so no two heights share a byte except the one percent the
    // percentiles already excluded.
    //
    // The expansion is centred on the field's median rather than on mid-grey.
    // The gain multiplies the distance from that median, so the height most of
    // the surface sits at stays where it is and only the relief around it
    // grows. Centring on mid-grey instead moved the whole surface toward white
    // as the gain rose - the other half of why high gain used to wash out.
    //
    // Histogram equalisation was measured on the same source as an
    // alternative to both: it does spread the flat tile faces over four times
    // the grey range (8.6 levels of variation inside a tile face became 41),
    // but what it spreads is the surface mottle, and the result reads as
    // speckle rather than relief. This keeps the source's contrast ratios and
    // only stops the clipping: the same face variation measures 17 levels at
    // gain 2 and 26 at gain 3.
    const double mid = std::clamp(levels.median, 0.02, 0.98);
    const double upper = 1.0 - mid;
    const double lower = mid;
    const double shoulder = clampedGain > 1.0 ? 0.15 * (1.0 - 1.0 / clampedGain) : 0.0;
    // Exponent that makes the shoulder leave the linear part at the same slope
    // it arrived with; only used when the shoulder exists.
    const double shoulderPower = shoulder > 1e-9
        ? (clampedGain - 1.0 + shoulder) / shoulder
        : 1.0;

    // 16-bit is the same mapping quantised finer, not a different one: the
    // levels and the gain keep meaning exactly what they mean at 8-bit, so the
    // two exports describe the same surface and only differ in how many steps
    // of it survive. On a photograph the height field itself spans far more
    // than 256 steps inside the percentile band, so at 8 bits neighbouring
    // samples collapse onto the same byte and the file is flatter than the
    // field was - which is the one thing an exported bump map must not be.
    const bool wide = bits > 8;
    QImage out(field.width,
               field.height,
               wide ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8);
    for (int y = 0; y < field.height; ++y) {
        uchar* line = out.scanLine(y);
        quint16* wideLine = wide ? reinterpret_cast<quint16*>(line) : nullptr;
        for (int x = 0; x < field.width; ++x) {
            const double level = (static_cast<double>(field.at(x, y)) - levels.low) * levels.stretch;
            double value = 0.0;
            if (shoulder <= 0.0) {
                value = 0.5 + (level - 0.5) * clampedGain;
            } else {
                const double scale = level >= mid ? upper : lower;
                const double u = scale > 1e-9 ? (level - mid) / scale : 0.0;
                double magnitude = std::abs(u) * clampedGain;
                if (magnitude > 1.0 - shoulder) {
                    const double t = std::min(1.0, (magnitude - (1.0 - shoulder))
                                                   / (clampedGain - (1.0 - shoulder)));
                    magnitude = (1.0 - shoulder)
                              + shoulder * (1.0 - std::pow(1.0 - t, shoulderPower));
                }
                const double shaped = u < 0.0 ? -magnitude : magnitude;
                value = mid + shaped * (shaped >= 0.0 ? upper : lower);
            }
            const double level01 = saturate(value);
            if (wideLine) {
                wideLine[x] = static_cast<quint16>(level01 * 65535.0 + 0.5);
            } else {
                // Untouched from before the 16-bit option existed: the 8-bit
                // bytes are the same ones every earlier release wrote.
                line[x] = static_cast<uchar>(level01 * 255.0);
            }
        }
    }
    return out;
}
}

TextureResult TextureGenerator::generate(const QImage& source, const TextureOptions& options, const QString& baseOutputDirectory)
{
    return buildResult(source, options, baseOutputDirectory, true);
}

TextureResult TextureGenerator::preview(const QImage& source, const TextureOptions& options)
{
    TextureOptions previewOptions = options;
    previewOptions.resolution = std::min(previewOptions.resolution, 1024);
    return buildResult(source, previewOptions, QString(), false);
}

bool TextureGenerator::isBackendAvailable(TextureBackendMode backendMode)
{
    switch (backendMode) {
    case TextureBackendMode::ClassicFast:
    case TextureBackendMode::EnhancedDetail:
        return true;
    case TextureBackendMode::AiBridge:
        // The ONNX Runtime DLL and the model are both optional at run time; the
        // UI greys the entry out and falls back when either is missing.
        return DepthEstimator::isAvailable();
    }

    return false;
}

TextureResult TextureGenerator::buildResult(const QImage& source, const TextureOptions& options, const QString& baseOutputDirectory, bool writeFiles)
{
    TextureResult result;
    result.jobId = writeFiles ? timeStampedJobId() : QStringLiteral("preview");

    if (writeFiles) {
        QDir().mkpath(baseOutputDirectory);
        result.outputDirectory = QDir(baseOutputDirectory).filePath(result.jobId);
        QDir().mkpath(result.outputDirectory);
    }

    if (source.isNull() || !anyMapEnabled(options)) {
        return result;
    }

    Profile profile;

    // Preparing the source is a third of a preview frame - the crop/scale plus,
    // when seamless is on, the pyramid blend - and it depends only on the source
    // and the output geometry. None of the sliders touch it, so during a drag
    // the source is byte-identical and every frame after the first was doing
    // the same work again to get the same pixels. Caching it is worth more than
    // any optimisation inside it: the first frame pays, the rest are free.
    //
    // The cache keeps a reference to the source image, which costs no memory
    // (QImage is implicitly shared) and keeps its cacheKey from being recycled
    // by another image while this entry is still valid.
    static QMutex cacheMutex;
    static QImage cachedSource;
    static qint64 cachedSourceKey = -1;
    static int cachedResolution = -1;
    static bool cachedSquare = false;
    static SquareFit cachedSquareFit = SquareFit::Crop;
    static bool cachedSeamless = false;
    static SeamlessMode cachedSeamlessMode = SeamlessMode::Blend;
    static bool cachedClipRepair = false;
    static QImage cachedPrepared;

    // Clipped highlights have to be rebuilt on the photograph, before the
    // seamless pass, because "blown" is evidence the seamless pass destroys:
    // its pyramid blend averages the two borders and drags a railed 255 core
    // down to 251, which is below any clip threshold worth having. Measured on
    // a real 1024 tile photo, the stage then found 15 pixels where the photo
    // had ten thousand, and the highlight came back as a flat grey disc. It
    // also fixes what the tile looks like: repairing after the blend repeats
    // the repaired highlight in all nine copies.
    const bool clipRepair = options.removeLighting && options.delightStrength > 0.0;

    QImage prepared;
    {
        QMutexLocker locker(&cacheMutex);
        const bool hit = !cachedPrepared.isNull()
            && cachedSourceKey == source.cacheKey()
            && cachedResolution == options.resolution
            && cachedSquare == options.square
            && cachedSquareFit == options.squareFit
            && cachedSeamless == options.seamless
            && cachedSeamlessMode == options.seamlessMode
            && cachedClipRepair == clipRepair;
        if (hit) {
            prepared = cachedPrepared;
        } else {
            prepared = prepareSource(source, options);
            profile.step("prepare");
            if (clipRepair) {
                // Unwrapped on purpose: the source does not tile yet, and
                // wrapping here would let the fill borrow donor pixels across
                // the border of a discontinuity that does not exist in reality.
                prepared = LightingRemoval::restoreClippedHighlights(prepared);
            }
            profile.step("cliprepair");
            if (options.seamless) {
                prepared = makeSeamless(prepared, options.seamlessMode);
            }
            profile.step("seamless");
            cachedPrepared = prepared;
            cachedSource = source;
            cachedSourceKey = source.cacheKey();
            cachedResolution = options.resolution;
            cachedSquare = options.square;
            cachedSquareFit = options.squareFit;
            cachedSeamless = options.seamless;
            cachedSeamlessMode = options.seamlessMode;
            cachedClipRepair = clipRepair;
        }
    }
    profile.step("source");

    result.size = prepared.size();

    const bool wrap = options.seamless;

    // The depth model is the expensive step, so it is deliberately gated: only
    // the AI backend asks for it, and only the final generate runs it unless the
    // user opted the preview in. Inference is cached inside DepthEstimator, so
    // once a source has been through it, changing a slider costs nothing.
    const bool neuralActive = options.backendMode == TextureBackendMode::AiBridge
        && (writeFiles || options.aiPreview)
        && DepthEstimator::isAvailable();
    // 322 is the preview size (23 x 14) - roughly a third of the pixels of 518
    // for a depth map that only has to look right at 512 px.
    const QImage depth = neuralActive ? DepthEstimator::estimate(prepared, writeFiles ? 518 : 322)
                                      : QImage();

    // De-lighting replaces the base color outright, so everything downstream -
    // bump, normal, roughness, metal, AO - is derived from the lit-free version
    // rather than from the photo. It runs before the height map is built, and it
    // uses the depth map purely as an edge guide when one is available.
    if (options.removeLighting && options.delightStrength > 0.0) {
        const QImage delit = LightingRemoval::remove(prepared, depth, options.delightStrength, wrap);
        if (!delit.isNull()) {
            prepared = delit;
        }
    }

    // Anime stylization targets the base color (and roughness) only: the height
    // map still derives from the original detail so the surface relief stays
    // honest instead of picking up fake contours from the posterized bands.
    QImage baseColor = prepared;
    if (options.styleMode == StyleMode::Anime) {
        baseColor = applyAnimeStyle(prepared);
    }

    if (options.generateBaseColor) {
        storeImage(result,
                   QStringLiteral("base"),
                   QStringLiteral("base_color.png"),
                   baseColor,
                   writeFiles);
    }

    HeightField height = createHeightField(prepared, options.bumpStrength, options.backendMode, wrap);
    profile.step("height");
    if (!depth.isNull()) {
        height = mergeDepthRelief(height, depth, options.bumpStrength, wrap);
    }
    if (options.generateBump) {
        // Auto-levels here too, so the bump map and the normal map describe the
        // same relief at the same contrast instead of the bump keeping whatever
        // contrast the photograph happened to have.
        const HeightLevels levels = heightLevels(height);
        storeImage(result,
                   QStringLiteral("bump"),
                   QStringLiteral("bump.png"),
                   heightFieldToImage(height, levels, options.bumpStrength, options.bumpBits),
                   writeFiles);
    }

    if (options.generateNormal) {
        // The relief slider and the normal strength are one control as far as
        // the map is concerned: createNormalMap() auto-levels the height before
        // it differentiates, which removes the amplitude the bump slider put
        // there. Feeding the bump strength into the gain again is what keeps
        // that slider moving the normals.
        const QImage normal = createNormalMap(height, options.normalStrength * options.bumpStrength,
                                              options.backendMode, wrap);
        profile.step("normal");
        storeImage(result,
                   QStringLiteral("normal"),
                   QStringLiteral("normal.png"),
                   normal,
                   writeFiles);
    }

    // In anime style the roughness map is biased toward a smooth, plastic-like
    // surface (cell-shaded materials read as low roughness); the per-pixel
    // roughness from the height map is still used, just pulled down. The height
    // (not the posterized base) feeds the roughness, so the color bands do not
    // introduce fake micro-contrast.
    const QImage roughness = createRoughnessMap(prepared, height, options.backendMode, wrap);
    QImage roughnessOut = roughness;
    if (options.styleMode == StyleMode::Anime) {
        roughnessOut = QImage(roughness.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < roughness.height(); ++y) {
            const uchar* srcLine = roughness.constScanLine(y);
            uchar* outLine = roughnessOut.scanLine(y);
            for (int x = 0; x < roughness.width(); ++x) {
                outLine[x] = static_cast<uchar>(srcLine[x] * 0.55);
            }
        }
    }
    if (options.generateRoughness) {
        storeImage(result,
                   QStringLiteral("roughness"),
                   QStringLiteral("roughness.png"),
                   roughnessOut,
                   writeFiles);
    }

    if (options.generateMetal) {
        const QImage metal = createMetalMap(prepared, roughness, options.backendMode);
        storeImage(result,
                   QStringLiteral("metal"),
                   QStringLiteral("metal.png"),
                   metal,
                   writeFiles);
    }

    if (options.generateAo) {
        const QImage ao = createAoMap(prepared, height, options.backendMode, wrap);
        storeImage(result,
                   QStringLiteral("ao"),
                   QStringLiteral("ao.png"),
                   ao,
                   writeFiles);
    }

    if (options.generateAlpha) {
        const QImage alpha = createAlphaMap(prepared);
        storeImage(result,
                   QStringLiteral("alpha"),
                   QStringLiteral("alpha.png"),
                   alpha,
                   writeFiles);
    }

    return result;
}

// Square by synthesis: keep the picture and grow the short side with the patch
// engine. This is the only one of the fits that puts new content in, so it is
// the one to pick when a photograph has to fill the square and cropping would
// lose the subject - and the one that stops being believable when the gap is
// large, because everything it adds is copied from what it can see.
QImage resynthesizeToSquare(const QImage& source)
{
    const int side = std::max(source.width(), source.height());
    const QImage src = source.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage canvas(w, side, QImage::Format_ARGB32);
    canvas.fill(Qt::black);
    for (int y = 0; y < h; ++y) {
        std::copy(src.constScanLine(y), src.constScanLine(y) + w * 4,
                  canvas.scanLine(y));
    }
    QImage mask(w, side, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = h; y < side; ++y) {
        uchar* line = mask.scanLine(y);
        std::memset(line, 255, static_cast<size_t>(w));
    }
    PatchSynthOptions options;
    options.vote = true;
    options.multiscale = true;
    options.iterations = 3;
    options.wrap = false; // the picture is the only context; there is nothing below
    QImage grown = patchSynthesis(canvas, mask, options);
    if (grown.isNull()) {
        grown = canvas;
    }
    return grown.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Square by content-aware resize - the idea Liquid Rescale is built on. Cut the
// least noticeable vertical (or horizontal) seams until the long side matches
// the short one. Every remaining pixel is the picture's own, at its own scale,
// which is what makes it work on a photograph; the price is that long straight
// edges visibly bend.
QImage seamCarveToSquare(const QImage& source)
{
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int finish = std::min(image.width(), image.height());
    const bool vertical = image.width() > image.height();

    // Flat working arrays rather than QImage::pixel(): the dynamic program runs
    // once per removed seam, and at 1024x768 that is 256 x 786k cells. Pixel
    // accessors turned that into 40 s; plain indexing keeps it near one.
    std::vector<unsigned char> lum;
    std::vector<double> energy;
    std::vector<double> cost;
    std::vector<int> seam;

    auto rebuild = [&]() {
        const int w = image.width();
        const int h = image.height();
        lum.assign(static_cast<size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            unsigned char* out = &lum[static_cast<size_t>(y) * w];
            for (int x = 0; x < w; ++x) {
                out[x] = static_cast<unsigned char>(qGray(line[x]));
            }
        }
        energy.assign(static_cast<size_t>(w) * h, 0.0);
        for (int y = 0; y < h; ++y) {
            const int y0 = std::max(0, y - 1);
            const int y1 = std::min(h - 1, y + 1);
            for (int x = 0; x < w; ++x) {
                const int x0 = std::max(0, x - 1);
                const int x1 = std::min(w - 1, x + 1);
                const double dx = static_cast<double>(lum[static_cast<size_t>(y) * w + x1])
                                - lum[static_cast<size_t>(y) * w + x0];
                const double dy = static_cast<double>(lum[static_cast<size_t>(y1) * w + x])
                                - lum[static_cast<size_t>(y0) * w + x];
                energy[static_cast<size_t>(y) * w + x] = std::abs(dx) + std::abs(dy);
            }
        }
    };

    auto findSeam = [&]() {
        const int w = image.width();
        const int h = image.height();
        // The seam advances along the long axis and picks a position across it:
        // a vertical seam walks down the rows choosing an x.
        const int along = vertical ? h : w;
        const int across = vertical ? w : h;
        auto index = [&](int a, int c) {
            const int x = vertical ? c : a;
            const int y = vertical ? a : c;
            return static_cast<size_t>(y) * w + x;
        };
        cost.assign(energy.size(), 0.0);
        for (int a = 0; a < along; ++a) {
            for (int c = 0; c < across; ++c) {
                double best = 0.0;
                if (a > 0) {
                    best = cost[index(a - 1, c)];
                    if (c > 0) {
                        best = std::min(best, cost[index(a - 1, c - 1)]);
                    }
                    if (c + 1 < across) {
                        best = std::min(best, cost[index(a - 1, c + 1)]);
                    }
                }
                cost[index(a, c)] = energy[index(a, c)] + best;
            }
        }
        seam.assign(static_cast<size_t>(along), 0);
        int bestC = 0;
        for (int c = 1; c < across; ++c) {
            if (cost[index(along - 1, c)] < cost[index(along - 1, bestC)]) {
                bestC = c;
            }
        }
        seam[static_cast<size_t>(along - 1)] = bestC;
        for (int a = along - 2; a >= 0; --a) {
            int c = seam[static_cast<size_t>(a + 1)];
            if (c > 0 && cost[index(a, c - 1)] < cost[index(a, c)]) {
                c -= 1;
            } else if (c + 1 < across && cost[index(a, c + 1)] < cost[index(a, c)]) {
                c += 1;
            }
            seam[static_cast<size_t>(a)] = c;
        }
    };

    while ((vertical ? image.width() : image.height()) > finish) {
        rebuild();
        findSeam();
        if (vertical) {
            const int w = image.width();
            const int h = image.height();
            QImage cut(w - 1, h, QImage::Format_ARGB32);
            for (int y = 0; y < h; ++y) {
                const QRgb* in = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                QRgb* out = reinterpret_cast<QRgb*>(cut.scanLine(y));
                const int skip = seam[static_cast<size_t>(y)];
                std::copy(in, in + skip, out);
                std::copy(in + skip + 1, in + w, out + skip);
            }
            image = cut;
        } else {
            const int w = image.width();
            const int h = image.height();
            QImage cut(w, h - 1, QImage::Format_ARGB32);
            std::vector<int> rowOf(static_cast<size_t>(w), 0);
            for (int x = 0; x < w; ++x) {
                rowOf[static_cast<size_t>(x)] = seam[static_cast<size_t>(x)];
            }
            for (int y = 0; y < h - 1; ++y) {
                QRgb* out = reinterpret_cast<QRgb*>(cut.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    // Source row for this x is either y or y+1 depending on
                    // whether its seam pixel has already been skipped.
                    const int sy = y < rowOf[static_cast<size_t>(x)] ? y : y + 1;
                    out[x] = image.pixel(x, sy);
                }
            }
            image = cut;
        }
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage TextureGenerator::prepareSource(const QImage& source, const TextureOptions& options)
{
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QSize targetSize;

    if (options.square) {
        if (options.squareFit == SquareFit::Crop) {
            image = cropToSquare(image);
        } else if (options.squareFit == SquareFit::Mirror) {
            image = mirrorToSquare(image);
        } else if (options.squareFit == SquareFit::Resynthesize) {
            image = resynthesizeToSquare(image);
        } else if (options.squareFit == SquareFit::SeamCarve) {
            image = seamCarveToSquare(image);
        }
        targetSize = QSize(options.resolution, options.resolution);
    } else {
        const double ratio = static_cast<double>(image.width()) / static_cast<double>(image.height());
        if (ratio >= 1.0) {
            targetSize = QSize(options.resolution, std::max(64, static_cast<int>(std::round(options.resolution / ratio))));
        } else {
            targetSize = QSize(std::max(64, static_cast<int>(std::round(options.resolution * ratio))), options.resolution);
        }
    }

    return image.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

static QImage seamlessAxis(const QImage& image, SeamAxis axis)
{
    // Laplacian-pyramid seamless (Burt & Adelson multiresolution spline).
    //
    // Offset-blend averages the whole image against a half-shifted copy once;
    // that hides the seam but doubles the high-frequency detail near the edge
    // (ghosting). Here we decompose into frequency bands (a Laplacian pyramid),
    // blend each band with a mask whose transition width matches that band's
    // scale, then recombine:
    //   - coarse bands (color patches) get a wide feather -> smooth colour seam;
    //   - fine bands (grain, brush strokes) get a narrow mask -> detail snaps to
    //     one side or the other instead of ghosting.
    // Every shift/blur/upsample wraps, so the output tiles exactly.
    const QImage original = image.convertToFormat(QImage::Format_ARGB32);
    const int width = original.width();
    const int height = original.height();
    if (width < 8 || height < 8) {
        return original.copy();
    }

    // --- 1. Half-shifted copy (the thing we feather between). ---------------
    // One axis at a time. A half-shift in *both* axes leaves the shifted copy
    // with two discontinuity lines (one per axis), and any region that spans
    // the tile - which is what a border ring is - has to cross one of them.
    // Shifting on a single axis leaves exactly one such line, and the region
    // that uses the shifted copy is then two strips on either side of it.
    const bool horizontal = axis == SeamAxis::Horizontal;
    const int halfX = horizontal ? width / 2 : 0;
    const int halfY = horizontal ? 0 : height / 2;
    auto shiftPixel = [&](int x, int y) {
        return original.pixel(wrapIndex(x + halfX, width), wrapIndex(y + halfY, height));
    };

    // --- 2. Pyramids of the original and the shifted copy. ------------------
    // level 0 = full resolution; each subsequent level is half size.
    struct Level
    {
        QVector<QRgb> i; // original, this level
        QVector<QRgb> s; // shifted,  this level
        QVector<QRgb> mask; // per-level blend mask
    };
    QVector<Level> levels;

    constexpr int kMaxLevels = 8;
    const int levelCount = std::max(1, std::min(kMaxLevels,
        static_cast<int>(std::floor(std::log2(std::min(width, height) / 4.0)))));

    // Source image for pyramid building. Re-sampled each level so every band
    // operates at a consistent scale.
    QVector<QRgb> srcI(static_cast<size_t>(width) * height);
    QVector<QRgb> srcS(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QRgb p = original.pixel(x, y);
            const QRgb s = shiftPixel(x, y);
            srcI[y * width + x] = p;
            srcS[y * width + x] = s;
        }
    }

    // --- 2b. Where the two copies meet. ------------------------------------
    // The switch has to happen somewhere, and a fixed ring around the border
    // (what this used to do) cuts through whatever is there: every grout line
    // it crosses is split, and the eye finds it immediately. The minimum cut
    // through the difference of the two copies puts the switch where they
    // already agree, so it costs nothing where it can be helped.
    std::vector<float> maskCur(static_cast<size_t>(width) * height, 1.0f);
    const QImage cutMask = buildSeamlessCutMask(original, axis);
    if (!cutMask.isNull()) {
        for (int y = 0; y < height; ++y) {
            const uchar* line = cutMask.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                maskCur[static_cast<size_t>(y) * width + x] = line[x] / 255.0f;
            }
        }
    } else {
        // Degenerate shapes (extreme aspect ratios leave no room for an
        // interior) fall back to a fixed cross-fade along this axis. Worse, but
        // always valid, and it still tiles in the axis it was given.
        const double transition = std::max(2.0, std::min(width, height) / 12.0);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double d = horizontal ? std::min(x, width - 1 - x) : std::min(y, height - 1 - y);
                const double t = std::clamp(d / transition, 0.0, 1.0);
                maskCur[static_cast<size_t>(y) * width + x] = static_cast<float>(t * t * (3.0 - 2.0 * t));
            }
        }
    }

    int lw = width;
    int lh = height;
    for (int level = 0; level < levelCount; ++level) {
        Level l;
        l.i = srcI.mid(0, static_cast<int>(srcI.size()));
        l.s = srcS.mid(0, static_cast<int>(srcS.size()));

        // The feather has to follow the band it is applied to. Blurring by a
        // fixed radius measured in *level* pixels makes the transition 3 px
        // wide at full resolution (fine detail snaps to one copy, no doubling)
        // and a third of the tile at the coarsest level (the overall colour
        // cross-fades, which is what the multiresolution spline is for).
        //
        // The previous mask was a fixed fraction of the tile at every level, so
        // every band crossed over the same ~40 image pixels - that is exactly
        // the width over which structured detail was visible twice.
        l.mask.resize(static_cast<size_t>(lw) * lh);
        const std::vector<float> soft = blurMask(maskCur, lw, lh, 1);
        for (int i = 0; i < lw * lh; ++i) {
            const int v = static_cast<int>(std::lround(std::clamp(soft[static_cast<size_t>(i)], 0.0f, 1.0f) * 255.0f));
            l.mask[i] = qRgb(v, v, v);
        }
        levels.append(l);

        // Build the next (half-size) level with a 2x2 box downsample.
        if (level == levelCount - 1) {
            break;
        }
        const int nw = lw / 2;
        const int nh = lh / 2;
        QVector<QRgb> nextI(static_cast<size_t>(nw) * nh);
        QVector<QRgb> nextS(static_cast<size_t>(nw) * nh);
        std::vector<float> nextMask(static_cast<size_t>(nw) * nh, 0.0f);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                int rI = 0, gI = 0, bI = 0, aI = 0;
                int rS = 0, gS = 0, bS = 0, aS = 0;
                float maskSum = 0.0f;
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = std::min(lh - 1, y * 2 + dy);
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(lw - 1, x * 2 + dx);
                        const QRgb pi = srcI[sy * lw + sx];
                        const QRgb ps = srcS[sy * lw + sx];
                        rI += qRed(pi); gI += qGreen(pi); bI += qBlue(pi); aI += qAlpha(pi);
                        rS += qRed(ps); gS += qGreen(ps); bS += qBlue(ps); aS += qAlpha(ps);
                        maskSum += maskCur[static_cast<size_t>(sy) * lw + sx];
                    }
                }
                const int idx = y * nw + x;
                nextI[idx] = qRgba(rI / 4, gI / 4, bI / 4, aI / 4);
                nextS[idx] = qRgba(rS / 4, gS / 4, bS / 4, aS / 4);
                nextMask[static_cast<size_t>(idx)] = maskSum * 0.25f;
            }
        }
        srcI = nextI;
        srcS = nextS;
        maskCur = std::move(nextMask);
        lw = nw;
        lh = nh;
    }

    // --- 3. Recombine coarse-to-fine. ---------------------------------------
    // Start from the coarsest level (a near-uniform patch, fully feathered),
    // then repeatedly upsample and add back the detail bands. At each level the
    // band contribution is mask-blended between "from original" and "from
    // shifted", so the final image holds the fine detail of whichever copy is
    // locally closer to the viewer while colour continuity comes from the
    // coarse blend. The coarsest level has no shift to resolve (its scale is
    // the whole tile), so it is taken from the original unchanged.
    const int coarseW = width >> (levels.size() - 1);
    const int coarseH = height >> (levels.size() - 1);
    QVector<QRgb> acc(static_cast<size_t>(coarseW) * coarseH);
    const Level& coarsest = levels.last();
    for (int y = 0; y < coarseH; ++y) {
        for (int x = 0; x < coarseW; ++x) {
            // The coarsest level is the overall colour, and it used to be taken
            // from the source alone. That left the colour of one copy sitting
            // under the detail of the other, which is a colour step at the seam
            // that no amount of feathered detail can hide. It gets blended too.
            const int i = y * coarseW + x;
            const double m = qRed(coarsest.mask[i]) / 255.0;
            const QRgb a = coarsest.i[i];
            const QRgb b = coarsest.s[i];
            acc[i] = qRgba(static_cast<int>(std::lround(qRed(a) * m + qRed(b) * (1.0 - m))),
                           static_cast<int>(std::lround(qGreen(a) * m + qGreen(b) * (1.0 - m))),
                           static_cast<int>(std::lround(qBlue(a) * m + qBlue(b) * (1.0 - m))),
                           static_cast<int>(std::lround(qAlpha(a) * m + qAlpha(b) * (1.0 - m))));
        }
    }

    // Bilinear upsample of one level onto the next finer grid. Every source is
    // upsampled on its own: a detail band has to be measured against the same
    // copy's coarse level, never against the running blend.
    const auto upsampleTo = [](const QVector<QRgb>& src, int uw, int uh, int cw, int ch) {
        QVector<QRgb> out(static_cast<size_t>(cw) * ch);
        for (int y = 0; y < ch; ++y) {
            const double sy = y * 0.5 - 0.25;
            const int y0 = wrapIndex(static_cast<int>(std::floor(sy)), uh);
            const int y1 = wrapIndex(y0 + 1, uh);
            const double fy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
            for (int x = 0; x < cw; ++x) {
                const double sx = x * 0.5 - 0.25;
                const int x0 = wrapIndex(static_cast<int>(std::floor(sx)), uw);
                const int x1 = wrapIndex(x0 + 1, uw);
                const double fx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
                const QRgb p00 = src[y0 * uw + x0];
                const QRgb p10 = src[y0 * uw + x1];
                const QRgb p01 = src[y1 * uw + x0];
                const QRgb p11 = src[y1 * uw + x1];
                auto mix = [&](double v00, double v10, double v01, double v11) {
                    return v00 * (1.0 - fx) * (1.0 - fy) + v10 * fx * (1.0 - fy)
                         + v01 * (1.0 - fx) * fy + v11 * fx * fy;
                };
                out[y * cw + x] = qRgba(
                    static_cast<int>(std::round(mix(qRed(p00), qRed(p10), qRed(p01), qRed(p11)))),
                    static_cast<int>(std::round(mix(qGreen(p00), qGreen(p10), qGreen(p01), qGreen(p11)))),
                    static_cast<int>(std::round(mix(qBlue(p00), qBlue(p10), qBlue(p01), qBlue(p11)))),
                    static_cast<int>(std::round(mix(qAlpha(p00), qAlpha(p10), qAlpha(p01), qAlpha(p11)))));
            }
        }
        return out;
    };

    // Build from the second-coarsest level down to level 0.
    for (int level = levels.size() - 2; level >= 0; --level) {
        const Level& l = levels[level];
        const Level& coarser = levels[level + 1];
        const int cw = width >> level;
        const int ch = height >> level;
        const int uw = cw / 2;
        const int uh = ch / 2;

        const QVector<QRgb> upAcc = upsampleTo(acc, uw, uh, cw, ch);
        const QVector<QRgb> upI = upsampleTo(coarser.i, uw, uh, cw, ch);
        const QVector<QRgb> upS = upsampleTo(coarser.s, uw, uh, cw, ch);

        // acc grows from the coarsest size to this level's size as we go up the
        // pyramid. The upsamples above only read the old (2x smaller) buffers,
        // so it is safe to grow it here before writing the blended band back.
        acc.resize(static_cast<size_t>(cw) * ch);

        // Detail band = this level minus *this copy's* upsampled coarser level.
        // Blend the two bands via the mask, then add them onto the upsampled
        // accumulator.
        //
        // Subtracting the running blend here instead - which is what this used
        // to do - cancels itself out one level later:
        //   up + m*(i-up) + (1-m)*(s-up) == m*i + (1-m)*s
        // so every level threw away the one above it and the whole pyramid
        // collapsed into a single crossfade over the three-pixel mask. On a
        // texture carrying a lighting gradient that left the two half-shifted
        // copies sitting side by side as a visible 2x2 checkerboard.
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const int idx = y * cw + x;
                const double m = qRed(l.mask[idx]) / 255.0; // mask value 0..1
                const QRgb pi = l.i[idx];
                const QRgb ps = l.s[idx];
                const QRgb ui = upI[idx];
                const QRgb us = upS[idx];
                const QRgb ua = upAcc[idx];
                const double blend_r = (qRed(pi) - qRed(ui)) * m + (qRed(ps) - qRed(us)) * (1.0 - m);
                const double blend_g = (qGreen(pi) - qGreen(ui)) * m + (qGreen(ps) - qGreen(us)) * (1.0 - m);
                const double blend_b = (qBlue(pi) - qBlue(ui)) * m + (qBlue(ps) - qBlue(us)) * (1.0 - m);
                const double blend_a = (qAlpha(pi) - qAlpha(ui)) * m + (qAlpha(ps) - qAlpha(us)) * (1.0 - m);
                acc[idx] = qRgba(
                    static_cast<int>(std::clamp(qRed(ua) + blend_r, 0.0, 255.0)),
                    static_cast<int>(std::clamp(qGreen(ua) + blend_g, 0.0, 255.0)),
                    static_cast<int>(std::clamp(qBlue(ua) + blend_b, 0.0, 255.0)),
                    static_cast<int>(std::clamp(qAlpha(ua) + blend_a, 0.0, 255.0)));
            }
        }
    }

    // --- 4. Write back to QImage. -------------------------------------------
    QImage output(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        QRgb* outLine = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < width; ++x) {
            outLine[x] = acc[y * width + x];
        }
    }
    return output;
}

// The seam band, synthesized from the rest of the picture.
//
// Only the band around the wrap boundary is replaced: everywhere else the
// photograph survives untouched, which is the difference between this and a
// full synthesis pass. The band is wide enough that patches sitting on the seam
// have known content to match on both sides, and narrow enough that the
// picture the user supplied is still what they see.
static QImage seamlessAxisPatch(const QImage& image, SeamAxis axis, const PatchSynthOptions& options)
{
    const bool horizontal = axis == SeamAxis::Horizontal;
    const int w = image.width();
    const int h = image.height();
    const int along = axis == SeamAxis::Horizontal ? w : h;
    // Ten percent of the short side, clamped: under ~32px there is too little
    // context for the match to be meaningful, over ~192 it starts replacing
    // content the eye would notice missing.
    const int band = std::clamp(std::min(w, h) / 10, 16, 192);

    // Only the *left* side of the wrap is rewritten, not both sides. The two
    // borders of a filled band are what decide whether it looks pasted, and one
    // side of a one-sided band is the picture's own edge: with a wrapped search
    // the match at x=0 sees x=w-1..w-R as known context, so the fill is pulled
    // into agreement with the edge it has to join. Rewriting both sides leaves
    // the wrap seam between two synthesized pixels with nothing to agree on -
    // measured 29 levels of step on a gradient, where this gives 0.
    QImage mask(w, h, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = 0; y < h; ++y) {
        uchar* line = mask.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int coord = axis == SeamAxis::Horizontal ? x : y;
            if (coord < band) {
                line[x] = 255;
            }
        }
    }
    QImage filled = patchSynthesis(image, mask, options);
    if (filled.isNull()) {
        return filled;
    }

    // The patch that filled the band came from somewhere else in the picture,
    // so it arrives at that place's brightness, not this one's. On a smooth ramp
    // that is a step where the band meets the photograph - measured 13 levels
    // with the copy mode and 32 with GPNN, and 48 at the wrap.
    if (band * 2 < along) {
        const QImage original = image.convertToFormat(QImage::Format_ARGB32);
        QImage output = filled.convertToFormat(QImage::Format_ARGB32);
        const int across = horizontal ? output.height() : output.width();
        auto pixelAt = [&](const QImage& img, int a, int c) {
            return horizontal ? img.pixel(a, c) : img.pixel(c, a);
        };
        // Re-level by membrane subtraction, the Poisson way.
        //
        // The fill arrives at whatever brightness its patches came from, so it
        // does not meet the photograph on either side of the band. Both sides
        // are known: the band's inner edge touches the photo at `band`, and its
        // outer edge touches the photo's last column through the wrap. Build
        // the smooth surface the photo asks for (a straight ramp between those
        // two boundaries per line), build the one the fill only thinks it wants
        // (a straight ramp between the fill's own two edges), and hand the fill
        // back with the difference of the two added. The fill's detail - the
        // part that is not its own membrane, i.e. everything the patch match
        // was for - survives untouched, and both of its borders land exactly on
        // the photo by construction (at a=0 and a=band-1 the detail term is
        // zero). Measured on a gradient: 48 levels of seam at the wrap before,
        // 0 after.
        // Both ends have to be exact, not smoothed: a=0 becomes the photo's
        // last column and a=band-1 its pixel at `band`, and that equality *is*
        // the continuity. Smoothing the ends along c (which an earlier version
        // did to keep noise out of the ramp) put 5 levels back into the seam
        // wherever the fill's edge happened to disagree with its neighbours.
        auto boundary = [&](const QImage& img, int a, int c, int ch) {
            const QRgb px = pixelAt(img, a, c);
            return static_cast<double>(ch == 0 ? qRed(px) : ch == 1 ? qGreen(px) : qBlue(px));
        };
        // Snapshot the fill's own edges first: the loop overwrites them as it
        // goes, and the ramp needs the values the synthesis produced.
        std::vector<float> fillStart(static_cast<size_t>(across) * 3, 0.0f);
        std::vector<float> fillEnd(fillStart.size(), 0.0f);
        for (int c = 0; c < across; ++c) {
            for (int ch = 0; ch < 3; ++ch) {
                fillStart[static_cast<size_t>(c) * 3 + ch] = boundary(output, 0, c, ch);
                fillEnd[static_cast<size_t>(c) * 3 + ch] = boundary(output, band - 1, c, ch);
            }
        }
        for (int a = 0; a < band; ++a) {
            const double t = band > 1 ? static_cast<double>(a) / (band - 1) : 1.0;
            for (int c = 0; c < across; ++c) {
                const QRgb base = pixelAt(output, a, c);
                int rgb[3] = { qRed(base), qGreen(base), qBlue(base) };
                for (int ch = 0; ch < 3; ++ch) {
                    const double photoRamp = boundary(original, along - 1, c, ch) * (1.0 - t)
                                           + boundary(original, band, c, ch) * t;
                    const double fillRamp = fillStart[static_cast<size_t>(c) * 3 + ch] * (1.0 - t)
                                          + fillEnd[static_cast<size_t>(c) * 3 + ch] * t;
                    const double value = rgb[ch] + photoRamp - fillRamp;
                    rgb[ch] = std::clamp(static_cast<int>(std::lround(value)), 0, 255);
                }
                const QRgb value = qRgba(rgb[0], rgb[1], rgb[2], qAlpha(base));
                if (horizontal) {
                    output.setPixel(a, c, value);
                } else {
                    output.setPixel(c, a, value);
                }
            }
        }
        return output;
    }
    return filled;
}

// Poisson-style level correction for an arbitrary hole.
//
// The synthesis fills a hole with patches that came from somewhere else, so the
// fill sits at *that* place's brightness and steps where it meets the picture.
// The only thing that has to be true is that the border closes; what is inside
// is free. So solve for a smooth correction g that equals (photo - fill) on the
// ring of hole pixels touching known content, add it back, and the border lands
// exactly on the photo while the fill's detail - which g, being smooth, does
// not touch - survives. For a band the solution is a straight ramp and this is
// the one-dimensional case; for the cross-shaped hole the Resynthesizer mode
// leaves, nothing one-dimensional exists at all.
//
// Solved coarse to fine: a plain relaxation on a 100-pixel-wide hole needs tens
// of thousands of sweeps, and each level here starts from the level below's
// answer, so sixty sweeps per level is enough.
void levelCorrectHole(QImage& filled, const QImage& photo, const QImage& mask, bool wrap)
{
    const int w = filled.width();
    const int h = filled.height();
    if (w < 4 || h < 4) {
        return;
    }
    std::vector<unsigned char> hole(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        const uchar* line = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            hole[static_cast<size_t>(y) * w + x] = line[x] > 127 ? 1 : 0;
        }
    }

    struct Correction
    {
        int w = 0;
        int h = 0;
        std::vector<float> value;           // three per pixel
        std::vector<unsigned char> fixed;   // ring pixel, value given
        std::vector<unsigned char> hole;
    };

    Correction finest;
    finest.w = w;
    finest.h = h;
    finest.value.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    finest.fixed.assign(static_cast<size_t>(w) * h, 0);
    finest.hole = hole;

    bool anyFixed = false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t index = static_cast<size_t>(y) * w + x;
            if (hole[index] == 0) {
                continue;
            }
            const QRgb fillPixel = filled.pixel(x, y);
            double sum[3] = { 0.0, 0.0, 0.0 };
            int count = 0;
            const int nx[4] = { x - 1, x + 1, x, x };
            const int ny[4] = { y, y, y - 1, y + 1 };
            for (int k = 0; k < 4; ++k) {
                int xx = nx[k];
                int yy = ny[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
                    if (!wrap) {
                        continue;
                    }
                    xx = wrapIndex(xx, w);
                    yy = wrapIndex(yy, h);
                }
                if (hole[static_cast<size_t>(yy) * w + xx] != 0) {
                    continue;
                }
                const QRgb known = photo.pixel(xx, yy);
                sum[0] += qRed(known) - qRed(fillPixel);
                sum[1] += qGreen(known) - qGreen(fillPixel);
                sum[2] += qBlue(known) - qBlue(fillPixel);
                ++count;
            }
            if (count == 0) {
                continue;
            }
            finest.value[index * 3 + 0] = static_cast<float>(sum[0] / count);
            finest.value[index * 3 + 1] = static_cast<float>(sum[1] / count);
            finest.value[index * 3 + 2] = static_cast<float>(sum[2] / count);
            finest.fixed[index] = 1;
            anyFixed = true;
        }
    }
    if (!anyFixed) {
        return;
    }

    std::vector<Correction> levels;
    levels.push_back(std::move(finest));
    while (levels.back().w >= 16 && levels.back().h >= 16 && levels.size() < 4) {
        const Correction& src = levels.back();
        Correction next;
        next.w = std::max(1, src.w / 2);
        next.h = std::max(1, src.h / 2);
        next.value.assign(static_cast<size_t>(next.w) * next.h * 3, 0.0f);
        next.fixed.assign(static_cast<size_t>(next.w) * next.h, 0);
        next.hole.assign(static_cast<size_t>(next.w) * next.h, 0);
        for (int y = 0; y < next.h; ++y) {
            for (int x = 0; x < next.w; ++x) {
                const size_t di = static_cast<size_t>(y) * next.w + x;
                double sum[3] = { 0.0, 0.0, 0.0 };
                double weight = 0.0;
                bool anyHole = false;
                bool anyFixed2 = false;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(src.w - 1, x * 2 + dx);
                        const int sy = std::min(src.h - 1, y * 2 + dy);
                        const size_t si = static_cast<size_t>(sy) * src.w + sx;
                        anyHole = anyHole || src.hole[si] != 0;
                        if (src.fixed[si] == 0) {
                            continue;
                        }
                        anyFixed2 = true;
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += src.value[si * 3 + c];
                        }
                        weight += 1.0;
                    }
                }
                next.hole[di] = anyHole ? 1 : 0;
                if (!anyFixed2) {
                    continue;
                }
                next.fixed[di] = 1;
                for (int c = 0; c < 3; ++c) {
                    next.value[di * 3 + c] = static_cast<float>(sum[c] / weight);
                }
            }
        }
        levels.push_back(std::move(next));
    }

    auto relax = [](Correction& level, int sweeps) {
        std::vector<float> next(level.value.size(), 0.0f);
        for (int sweep = 0; sweep < sweeps; ++sweep) {
            next = level.value;
            for (int y = 0; y < level.h; ++y) {
                for (int x = 0; x < level.w; ++x) {
                    const size_t index = static_cast<size_t>(y) * level.w + x;
                    if (level.hole[index] == 0 || level.fixed[index] != 0) {
                        continue;
                    }
                    float sum[3] = { 0.0f, 0.0f, 0.0f };
                    int count = 0;
                    const int nx[4] = { x - 1, x + 1, x, x };
                    const int ny[4] = { y, y, y - 1, y + 1 };
                    for (int k = 0; k < 4; ++k) {
                        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= level.w || ny[k] >= level.h) {
                            continue;
                        }
                        const size_t ni = static_cast<size_t>(ny[k]) * level.w + nx[k];
                        if (level.hole[ni] == 0) {
                            continue;
                        }
                        sum[0] += level.value[ni * 3 + 0];
                        sum[1] += level.value[ni * 3 + 1];
                        sum[2] += level.value[ni * 3 + 2];
                        ++count;
                    }
                    if (count == 0) {
                        continue;
                    }
                    next[index * 3 + 0] = sum[0] / count;
                    next[index * 3 + 1] = sum[1] / count;
                    next[index * 3 + 2] = sum[2] / count;
                }
            }
            std::swap(level.value, next);
        }
    };

    relax(levels.back(), 400);
    for (size_t index = levels.size() - 1; index-- > 0;) {
        Correction& coarser = levels[index + 1];
        Correction& level = levels[index];
        // Bilinear from the coarser answer, then let the finer ring values pull
        // it into place.
        Correction seeded = level;
        for (int y = 0; y < level.h; ++y) {
            const float fy = (y + 0.5f) * 0.5f - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, coarser.h - 1);
            const int y1 = std::min(coarser.h - 1, y0 + 1);
            const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
            for (int x = 0; x < level.w; ++x) {
                const float fx = (x + 0.5f) * 0.5f - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, coarser.w - 1);
                const int x1 = std::min(coarser.w - 1, x0 + 1);
                const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
                const size_t di = static_cast<size_t>(y) * level.w + x;
                if (level.hole[di] == 0 || level.fixed[di] != 0) {
                    continue;
                }
                const size_t i00 = (static_cast<size_t>(y0) * coarser.w + x0) * 3;
                const size_t i10 = (static_cast<size_t>(y0) * coarser.w + x1) * 3;
                const size_t i01 = (static_cast<size_t>(y1) * coarser.w + x0) * 3;
                const size_t i11 = (static_cast<size_t>(y1) * coarser.w + x1) * 3;
                for (int c = 0; c < 3; ++c) {
                    const float top = coarser.value[i00 + c] * (1.0f - tx) + coarser.value[i10 + c] * tx;
                    const float bottom = coarser.value[i01 + c] * (1.0f - tx) + coarser.value[i11 + c] * tx;
                    seeded.value[di * 3 + c] = top * (1.0f - ty) + bottom * ty;
                }
            }
        }
        relax(seeded, 60);
        level.value = seeded.value;
    }

    QImage out = filled.convertToFormat(QImage::Format_ARGB32);
    const Correction& full = levels.front();
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const size_t index = static_cast<size_t>(y) * w + x;
            if (hole[index] == 0) {
                continue;
            }
            const QRgb base = line[x];
            line[x] = qRgba(
                std::clamp(static_cast<int>(std::lround(qRed(base) + full.value[index * 3 + 0])), 0, 255),
                std::clamp(static_cast<int>(std::lround(qGreen(base) + full.value[index * 3 + 1])), 0, 255),
                std::clamp(static_cast<int>(std::lround(qBlue(base) + full.value[index * 3 + 2])), 0, 255),
                qAlpha(base));
        }
    }
    filled = out;
}

// Seamless tiling, as two one-axis passes.
//
// The second pass runs on the output of the first. That is safe: shifting and
// pouring along y keeps every row's left-to-right continuity exactly as the
// first pass left it, so the horizontal tiling survives the vertical pass.
QImage TextureGenerator::makeSeamless(const QImage& image, SeamlessMode mode)
{
    if (image.isNull()) {
        return image;
    }
    if (mode == SeamlessMode::Mirror) {
        // Fold each axis onto its own first half. The result is symmetric about
        // both centres, and a symmetric tile wraps exactly: the first column
        // equals the last by the fold, the second equals the second-to-last, and
        // so on. No synthesis, no blur, nothing invented - the detail is the
        // picture's own, at the price of the mirror axis showing as a pattern.
        const QImage src = image.convertToFormat(QImage::Format_ARGB32);
        const int w = src.width();
        const int h = src.height();
        QImage folded(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            const int sy = y < h / 2 ? y : h - 1 - y;
            const QRgb* in = reinterpret_cast<const QRgb*>(src.constScanLine(sy));
            QRgb* out = reinterpret_cast<QRgb*>(folded.scanLine(y));
            for (int x = 0; x < w; ++x) {
                out[x] = in[x < w / 2 ? x : w - 1 - x];
            }
        }
        return folded;
    }
    if (mode == SeamlessMode::Resynthesizer) {
        // GIMP's Make Seamless. Rolling by half puts the wrap where the middle
        // of the picture used to be, so the discontinuity the user is trying to
        // hide is now a cross through the centre with real content on all four
        // of its sides - a much better posed search than a band pinned to the
        // edge. Roll in, fill the cross, roll back out.
        const QImage src = image.convertToFormat(QImage::Format_ARGB32);
        const int w = src.width();
        const int h = src.height();
        const int halfX = w / 2;
        const int halfY = h / 2;
        QImage rolled(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            const QRgb* in = reinterpret_cast<const QRgb*>(src.constScanLine((y + halfY) % h));
            QRgb* out = reinterpret_cast<QRgb*>(rolled.scanLine(y));
            for (int x = 0; x < w; ++x) {
                out[x] = in[(x + halfX) % w];
            }
        }
        const int band = std::clamp(std::min(w, h) / 10, 16, 192);
        if (band * 2 >= std::min(w, h)) {
            return image;
        }
        QImage mask(w, h, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 0; y < h; ++y) {
            uchar* line = mask.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const bool inColumn = x >= halfX - band / 2 && x < halfX + band / 2;
                const bool inRow = y >= halfY - band / 2 && y < halfY + band / 2;
                if (inColumn || inRow) {
                    line[x] = 255;
                }
            }
        }
        PatchSynthOptions options;
        options.vote = true; // the cross is wide; averaging is what holds it together
        options.multiscale = true;
        options.iterations = 3;
        QImage filled = patchSynthesis(rolled, mask, options);
        if (filled.isNull()) {
            return image;
        }
        // The synthesised cross comes from patches elsewhere in the picture, so
        // it arrives at that place's brightness and steps where it meets the
        // four sides. Close the border with the harmonic correction rather than
        // returning the step: for a band it would reduce to the same ramp the
        // other modes use, but here the hole has no one-dimensional answer.
        levelCorrectHole(filled, rolled, mask, false);
        const QImage flat = filled.convertToFormat(QImage::Format_ARGB32);
        QImage restored(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            const QRgb* in = reinterpret_cast<const QRgb*>(flat.constScanLine(y));
            QRgb* out = reinterpret_cast<QRgb*>(restored.scanLine((y - halfY + h) % h));
            for (int x = 0; x < w; ++x) {
                out[(x - halfX + w) % w] = in[x];
            }
        }
        return restored;
    }
    if (mode != SeamlessMode::Blend) {
        PatchSynthOptions options;
        options.wrap = true; // the band wraps across the seam by definition
        options.vote = mode == SeamlessMode::Gpnn;
        // Both modes run the pyramid. The difference between them is how a
        // pixel's patch is written back (best copy vs. average of every patch
        // over it), not how the search is organised: a flat search over a band
        // wider than the patch is the case patch match handles worst, and
        // measured 5.6 dB on the checkerboard where the pyramid gives 38.
        options.multiscale = true;
        options.iterations = mode == SeamlessMode::Gpnn ? 3 : 4;
        const QImage horizontal = seamlessAxisPatch(image, SeamAxis::Horizontal, options);
        const QImage verticalInput = horizontal.isNull() ? image : horizontal;
        const QImage both = seamlessAxisPatch(verticalInput, SeamAxis::Vertical, options);
        return both.isNull() ? verticalInput : both;
    }
    const QImage horizontal = seamlessAxis(image, SeamAxis::Horizontal);
    const QImage verticalInput = horizontal.isNull() ? image : horizontal;
    const QImage both = seamlessAxis(verticalInput, SeamAxis::Vertical);
    return both.isNull() ? verticalInput : both;
}

// Convert an image into an anime / cell-shaded base color:
//  1. posterize luminance into N soft bands (smoothstep at each band edge, so
//     the flat tints read as cel-shading instead of harsh dither);
//  2. preserve hue by scaling RGB by the luminance ratio;
//  3. detect edges on a lightly blurred luminance (wrap-aware) with a 3x3 Sobel
//     and darken them into outlines;
//  4. gently lift saturation for the typical anime palette.
QImage TextureGenerator::applyAnimeStyle(const QImage& image)
{
    const int width = image.width();
    const int height = image.height();
    const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);

    constexpr int bandCount = 5;
    constexpr double outlineStrength = 0.55; // restrained: Genshin-style edges, not US-cartoon ink
    constexpr double outlineThreshold = 0.10;
    constexpr double saturationBoost = 1.18;
    const QRgb outlineColor = qRgba(18, 20, 26, 255);

    auto posterize = [](double v, int bands) {
        // Hard quantization per channel: the signature flat cel-shaded tint.
        // 0..1 -> 0..(bands-1) step, snapped to the nearest band center.
        const int level = std::clamp(static_cast<int>(std::lround(v * (bands - 1))), 0, bands - 1);
        return level / static_cast<double>(bands - 1);
    };
    // Edge detection on the raw image, one Sobel per RGB channel, taking the
    // max magnitude. Posterized flat regions have constant channels (so no
    // outline), while any hard tint boundary (e.g. red over blue) triggers a
    // dark outline even when the luminance barely changes.
    auto edgeAt = [&](int x, int y) {
        auto channel = [&](int cx, int cy, int c) {
            const QRgb p = rgba.pixel(wrapIndex(cx, width), wrapIndex(cy, height));
            return c == 0 ? qRed(p) / 255.0 : (c == 1 ? qGreen(p) / 255.0 : qBlue(p) / 255.0);
        };
        double maxMag = 0.0;
        for (int c = 0; c < 3; ++c) {
            const double gx = (channel(x + 1, y - 1, c) + 2.0 * channel(x + 1, y, c) + channel(x + 1, y + 1, c))
                            - (channel(x - 1, y - 1, c) + 2.0 * channel(x - 1, y, c) + channel(x - 1, y + 1, c));
            const double gy = (channel(x - 1, y + 1, c) + 2.0 * channel(x, y + 1, c) + channel(x + 1, y + 1, c))
                            - (channel(x - 1, y - 1, c) + 2.0 * channel(x, y - 1, c) + channel(x + 1, y - 1, c));
            const double mag = std::sqrt(gx * gx + gy * gy) / 4.0;
            maxMag = std::max(maxMag, mag);
        }
        return maxMag;
    };

    QImage output(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        QRgb* outLine = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = srcLine[x];

            // 1. Posterize each channel to a flat tint band.
            double r = posterize(qRed(pixel) / 255.0, bandCount);
            double g = posterize(qGreen(pixel) / 255.0, bandCount);
            double b = posterize(qBlue(pixel) / 255.0, bandCount);

            // 2. Anime colour-temperature split: shadows drift cool (blue-violet,
            // the classic anime shadow tint) and highlights drift warm. This is
            // what makes flat tints read as "Genshin-style" rather than plain
            // poster art. The split ramps with luminance so it does not add a
            // visible boundary inside a tint.
            {
                const double l = r * 0.2126 + g * 0.7152 + b * 0.0722;
                const double cool = std::clamp((0.42 - l) / 0.20, 0.0, 1.0); // fully cool below ~0.22 luma
                const double warm = std::clamp((l - 0.55) / 0.20, 0.0, 1.0); // fully warm above ~0.75 luma
                r = r * (1.0 - cool * 0.10) + warm * 0.03;
                b = b * (1.0 - warm * 0.10) + cool * 0.10;
                g = g * (1.0 - (cool + warm) * 0.05) + warm * 0.02;
            }

            // 3. Edge outline from the max channel Sobel magnitude.
            const double magnitude = edgeAt(x, y);
            const double edge = std::clamp((magnitude - outlineThreshold) / outlineThreshold, 0.0, 1.0);

            if (edge > 0.0) {
                r = r * (1.0 - edge * outlineStrength) + qRed(outlineColor) / 255.0 * edge * outlineStrength;
                g = g * (1.0 - edge * outlineStrength) + qGreen(outlineColor) / 255.0 * edge * outlineStrength;
                b = b * (1.0 - edge * outlineStrength) + qBlue(outlineColor) / 255.0 * edge * outlineStrength;
            }

            // 4. Saturation lift around luma.
            const double lNew = r * 0.2126 + g * 0.7152 + b * 0.0722;
            r = lNew + (r - lNew) * saturationBoost;
            g = lNew + (g - lNew) * saturationBoost;
            b = lNew + (b - lNew) * saturationBoost;

            outLine[x] = qRgba(static_cast<int>(saturate(r) * 255.0),
                               static_cast<int>(saturate(g) * 255.0),
                               static_cast<int>(saturate(b) * 255.0),
                               qAlpha(pixel));
        }
    }

    return output;
}

HeightField TextureGenerator::createHeightField(const QImage& image, double strength, TextureBackendMode backendMode, bool wrap)
{
    // Luminance goes straight to float. Round-tripping it through an 8-bit grey
    // image first used to lose a level on 26% of pixels, because
    // luminance(c) * 255.0 lands just below c for 72 of the 256 grey values, and
    // the cast truncated. That is a bias, not dither: the height map was built
    // from an image that was systematically one level dark in places.
    HeightField luma;
    luma.reset(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            luma.at(x, y) = static_cast<float>(saturate(luminance(image.pixel(x, y))));
        }
    }

    // Multi-scale difference-of-Gaussians. Every blur is wrap-aware in seamless
    // mode so the height map stays tileable at its borders.
    const bool enhanced = backendMode == TextureBackendMode::EnhancedDetail;
    const HeightField blurFine = boxBlurField(luma, scaledRadius(1, image), wrap);
    const HeightField blurSmall = boxBlurField(luma, scaledRadius(enhanced ? 3 : 2, image), wrap);
    const HeightField blurMedium = boxBlurField(luma, scaledRadius(enhanced ? 8 : 6, image), wrap);
    const HeightField blurLarge = boxBlurField(luma, scaledRadius(enhanced ? 20 : 12, image), wrap);

    HeightField output;
    output.reset(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double base = luma.at(x, y);
            const double micro = base - blurFine.at(x, y);
            const double fine = blurFine.at(x, y) - blurSmall.at(x, y);
            const double medium = blurSmall.at(x, y) - blurMedium.at(x, y);
            const double large = blurMedium.at(x, y) - blurLarge.at(x, y);

            double merged;
            if (enhanced) {
                // The micro band is pure sensor noise in flat regions, so gate
                // it by how much local structure the fine band sees.
                const double noiseGuard = std::clamp(std::abs(fine) * 6.0, 0.0, 1.0);
                merged = 0.5
                    + micro * 0.85 * noiseGuard
                    + fine * 1.55
                    + medium * 1.05
                    + large * 0.62;
            } else {
                merged = 0.5 + fine * 1.20 + large * 0.55;
            }

            output.at(x, y) = static_cast<float>(saturate(0.5 + (merged - 0.5) * strength));
        }
    }

    return output;
}

HeightField TextureGenerator::mergeDepthRelief(const HeightField& heightMap, const QImage& depth, double strength, bool wrap)
{
    const QImage gray = depth.convertToFormat(QImage::Format_Grayscale8);
    if (gray.width() != heightMap.width || gray.height() != heightMap.height) {
        return heightMap;
    }

    HeightField depthField;
    depthField.reset(gray.width(), gray.height());
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            depthField.at(x, y) = line[x] / 255.0f;
        }
    }

    // Band-pass the depth map: subtracting a wide blur removes the global
    // gradient, which would otherwise tilt the entire surface, and leaves the
    // object-scale relief. Both blurs wrap in seamless mode so the tiling
    // property survives.
    const HeightField near = boxBlurField(depthField, scaledRadius(6, gray), wrap);
    const HeightField far = boxBlurField(depthField, scaledRadius(96, gray), wrap);

    // The band is near zero for a flat depth map - the model finding no geometry
    // is a legitimate answer, and in that case the luminance height map passes
    // through untouched rather than being flattened by a no-op depth map.
    constexpr double kReliefGain = 1.6;

    HeightField output;
    output.reset(heightMap.width, heightMap.height);
    for (int y = 0; y < heightMap.height; ++y) {
        for (int x = 0; x < heightMap.width; ++x) {
            const double relief = near.at(x, y) - far.at(x, y);
            output.at(x, y) = static_cast<float>(saturate(heightMap.at(x, y) + relief * kReliefGain * strength));
        }
    }
    return output;
}

QImage TextureGenerator::createNormalMap(const HeightField& heightMap, double strength, TextureBackendMode backendMode, bool wrap)
{
    const int width = heightMap.width;
    const int height = heightMap.height;
    QImage normal(width, height, QImage::Format_ARGB32_Premultiplied);
    if (width <= 0 || height <= 0) {
        return normal;
    }

    // Auto-levels on the height field, then a fixed gain. Normalising in the
    // height domain (rather than scaling by the slope distribution) is what
    // makes the result depend on the slider and the surface shape rather than
    // on how much contrast the photograph happened to have. see heightLevels()
    // for why the stretch is capped; the precision regression in
    // tg_height_precision measures exactly that cap.
    const HeightLevels levels = heightLevels(heightMap);
    const double low = levels.low;
    const double stretch = levels.stretch;

    auto sample = [&](int x, int y) {
        double value = 0.0;
        if (wrap) {
            value = static_cast<double>(heightMap.at(wrapIndex(x, width), wrapIndex(y, height)));
        } else {
            value = static_cast<double>(heightMap.at(std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1)));
        }
        return (value - low) * stretch;
    };

    // Sobel 3x3. A divisor of 4 keeps the magnitude comparable to the plain
    // central difference the first version used.
    auto gradient = [&](int x, int y, double& gx, double& gy) {
        gx = (sample(x + 1, y - 1) + 2.0 * sample(x + 1, y) + sample(x + 1, y + 1))
           - (sample(x - 1, y - 1) + 2.0 * sample(x - 1, y) + sample(x - 1, y + 1));
        gy = (sample(x - 1, y + 1) + 2.0 * sample(x, y + 1) + sample(x + 1, y + 1))
           - (sample(x - 1, y - 1) + 2.0 * sample(x, y - 1) + sample(x + 1, y - 1));
        gx /= 4.0;
        gy /= 4.0;
    };

    // Fixed gain, as a height-to-normal tool has always had it. What used to
    // be here normalised the *slope* distribution (mapping its 90th percentile
    // to a target angle, then clamping the result to at most six times a fixed
    // gain). That did make a flat photo respond to the slider, but it meant the
    // slider's absolute meaning still depended on the source, and on a noisy
    // image it amplified the noise into relief. The stretch above already
    // removes the source-contrast dependence, so this is a plain multiplier:
    // for a normally-exposed photo it gives the same relief the old lower bound
    // did, and the slider now means the same thing on every image.
    const double clampedStrength = std::clamp(strength, 0.0, 3.0);
    const double gain = clampedStrength * (backendMode == TextureBackendMode::EnhancedDetail ? 2.35 : 2.0);

    for (int y = 0; y < height; ++y) {
        QRgb* outLine = reinterpret_cast<QRgb*>(normal.scanLine(y));
        for (int x = 0; x < width; ++x) {
            double gx = 0.0;
            double gy = 0.0;
            gradient(x, y, gx, gy);

            double nx = -gx * gain;
            // Green follows the OpenGL/Blender convention (v up the image), the
            // same one the Normal Map node and glTF tangents expect. The row
            // index runs the other way, so the sign is +gy: a Blender bake of a
            // raised bump has G near 255 on the bump's upper side and near 0 on
            // its lower side, and -gy produced exactly the reverse - a DirectX
            // map, which lights inside-out in every Blender material while
            // looking plausible in isolation.
            double ny = gy * gain;
            double nz = 1.0;
            const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= length;
            ny /= length;
            nz /= length;

            const int r = static_cast<int>((nx * 0.5 + 0.5) * 255.0);
            const int g = static_cast<int>((ny * 0.5 + 0.5) * 255.0);
            const int b = static_cast<int>((nz * 0.5 + 0.5) * 255.0);
            outLine[x] = qRgba(r, g, b, 255);
        }
    }

    return normal;
}

QImage TextureGenerator::createRoughnessMap(const QImage& image, const HeightField& heightMap, TextureBackendMode backendMode, bool wrap)
{
    const HeightField fineBlur = boxBlurField(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 1 : 2, image), wrap);
    const HeightField broadBlur = boxBlurField(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 16 : 12, image), wrap);
    QImage output(image.size(), QImage::Format_Grayscale8);

    for (int y = 0; y < image.height(); ++y) {
        uchar* outLine = output.scanLine(y);

        for (int x = 0; x < image.width(); ++x) {
            const QColor color = QColor::fromRgba(image.pixel(x, y));
            const double luma = luminance(color.rgba());
            const double maxChannel = std::max({color.redF(), color.greenF(), color.blueF()});
            const double minChannel = std::min({color.redF(), color.greenF(), color.blueF()});
            const double saturation = maxChannel > 0.0001 ? (maxChannel - minChannel) / maxChannel : 0.0;
            const double microContrast = std::abs(heightMap.at(x, y) - fineBlur.at(x, y));
            const double macroContrast = std::abs(heightMap.at(x, y) - broadBlur.at(x, y));
            const double highlight = saturate((luma - 0.62) * (1.0 - saturation) * 1.9);

            double roughness = 0.32
                + saturation * 0.22
                + microContrast * 1.25
                + macroContrast * 0.45
                - highlight * 0.28;

            if (backendMode == TextureBackendMode::EnhancedDetail) {
                const double chromaPenalty = saturate((saturation - 0.2) * 0.55);
                roughness = 0.24
                    + saturation * 0.16
                    + microContrast * 1.65
                    + macroContrast * 0.62
                    - highlight * 0.30
                    + chromaPenalty * 0.10;
            }

            outLine[x] = static_cast<uchar>(saturate(std::clamp(roughness, 0.04, 0.98)) * 255.0);
        }
    }

    return output;
}

QImage TextureGenerator::createMetalMap(const QImage& image, const QImage& roughnessMap, TextureBackendMode backendMode)
{
    QImage output(image.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < image.height(); ++y) {
        const uchar* roughLine = roughnessMap.constScanLine(y);
        uchar* outLine = output.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = QColor::fromRgba(image.pixel(x, y));
            const double maxChannel = std::max({color.redF(), color.greenF(), color.blueF()});
            const double minChannel = std::min({color.redF(), color.greenF(), color.blueF()});
            const double saturation = maxChannel > 0.0001 ? (maxChannel - minChannel) / maxChannel : 0.0;
            const double luma = luminance(color.rgba());
            const double roughness = roughLine[x] / 255.0;

            double metallicBias = (1.0 - saturation) * 0.60
                + std::abs(luma - 0.5) * 0.55
                - roughness * 0.30;

            if (backendMode == TextureBackendMode::EnhancedDetail) {
                const double neutralScore = saturate(1.0 - saturation * 1.25);
                const double darkBrightScore = saturate(std::abs(luma - 0.5) * 1.8);
                metallicBias = neutralScore * 0.72
                    + darkBrightScore * 0.35
                    - roughness * 0.42;
                metallicBias *= saturate(1.08 - roughness * 0.45);
            }

            outLine[x] = static_cast<uchar>(saturate((metallicBias - 0.18) / 0.80) * 255.0);
        }
    }
    return output;
}

QImage TextureGenerator::createAoMap(const QImage& image, const HeightField& heightMap, TextureBackendMode backendMode, bool wrap)
{
    Q_UNUSED(image)

    const HeightField midBlur = boxBlurField(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 8 : 6, image), wrap);
    const HeightField largeBlur = boxBlurField(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 24 : 18, image), wrap);
    QImage output(QSize(heightMap.width, heightMap.height), QImage::Format_Grayscale8);

    for (int y = 0; y < heightMap.height; ++y) {
        uchar* outLine = output.scanLine(y);

        for (int x = 0; x < heightMap.width; ++x) {
            const double heightValue = heightMap.at(x, y);
            const double cavity = saturate((midBlur.at(x, y) - heightValue) * (backendMode == TextureBackendMode::EnhancedDetail ? 1.6 : 1.35));
            const double basin = saturate((largeBlur.at(x, y) - heightValue) * (backendMode == TextureBackendMode::EnhancedDetail ? 1.32 : 1.1));
            double ao = 1.0 - cavity * 0.72 - basin * 0.48;
            if (backendMode == TextureBackendMode::EnhancedDetail) {
                ao = 1.0 - cavity * 0.82 - basin * 0.56;
            }
            outLine[x] = static_cast<uchar>(saturate(std::clamp(ao, 0.14, 1.0)) * 255.0);
        }
    }

    return output;
}

QImage TextureGenerator::createAlphaMap(const QImage& image)
{
    QImage output(image.size(), QImage::Format_Grayscale8);
    bool hasMeaningfulAlpha = false;
    for (int y = 0; y < image.height() && !hasMeaningfulAlpha; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) < 250) {
                hasMeaningfulAlpha = true;
                break;
            }
        }
    }

    if (!hasMeaningfulAlpha) {
        output.fill(255);
        return output;
    }

    for (int y = 0; y < image.height(); ++y) {
        uchar* outLine = output.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            outLine[x] = static_cast<uchar>(qAlpha(image.pixel(x, y)));
        }
    }

    return output;
}

double TextureGenerator::luminance(const QRgb& color)
{
    return qRed(color) / 255.0 * 0.2126 + qGreen(color) / 255.0 * 0.7152 + qBlue(color) / 255.0 * 0.0722;
}


