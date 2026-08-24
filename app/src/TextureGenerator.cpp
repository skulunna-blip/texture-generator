#include "TextureGenerator.h"

#include <QDateTime>
#include <QDir>

#include <stdexcept>

#include <algorithm>
#include <cmath>

namespace
{
QImage cropToSquare(const QImage& source)
{
    const int side = std::min(source.width(), source.height());
    const QRect rect((source.width() - side) / 2, (source.height() - side) / 2, side, side);
    return source.copy(rect);
}

int wrapIndex(int index, int mod)
{
    int v = index % mod;
    return v < 0 ? v + mod : v;
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
        return false;
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

    if (!isBackendAvailable(options.backendMode)) {
        throw std::runtime_error("The AI material decomposition backend is not installed yet.");
    }

    if (source.isNull() || !anyMapEnabled(options)) {
        return result;
    }

    QImage prepared = prepareSource(source, options);
    if (options.seamless) {
        prepared = makeSeamless(prepared);
    }

    result.size = prepared.size();

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

    const bool wrap = options.seamless;

    const QImage height = createHeightMap(prepared, options.bumpStrength, options.backendMode, wrap);
    if (options.generateBump) {
        storeImage(result,
                   QStringLiteral("bump"),
                   QStringLiteral("bump.png"),
                   height,
                   writeFiles);
    }

    if (options.generateNormal) {
        const QImage normal = createNormalMap(height, options.normalStrength, options.backendMode, wrap);
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

QImage TextureGenerator::prepareSource(const QImage& source, const TextureOptions& options)
{
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QSize targetSize;

    if (options.square) {
        image = cropToSquare(image);
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

QImage TextureGenerator::makeSeamless(const QImage& image)
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
    const int halfX = width / 2;
    const int halfY = height / 2;
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

    int lw = width;
    int lh = height;
    for (int level = 0; level < levelCount; ++level) {
        Level l;
        l.i = srcI.mid(0, static_cast<int>(srcI.size()));
        l.s = srcS.mid(0, static_cast<int>(srcS.size()));

        // Per-level mask: distance from each pixel to the nearest edge, mapped
        // with smoothstep. The transition width scales with the band so coarse
        // levels blend widely and fine levels snap sharply. The mask is the same
        // for both copies (symmetric feather).
        l.mask.resize(static_cast<size_t>(lw) * lh);
        const double transition = std::max(2.0, std::min(lw, lh) / 12.0);
        for (int y = 0; y < lh; ++y) {
            const double dy = std::min(y, lh - 1 - y);
            for (int x = 0; x < lw; ++x) {
                const double dx = std::min(x, lw - 1 - x);
                const double dist = std::min(dx, dy) / transition;
                double t = std::clamp(dist, 0.0, 1.0);
                t = t * t * (3.0 - 2.0 * t); // smoothstep: 0 at edge, 1 at centre
                l.mask[y * lw + x] = qRgb(static_cast<int>(t * 255.0),
                                          static_cast<int>(t * 255.0),
                                          static_cast<int>(t * 255.0));
            }
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
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                int rI = 0, gI = 0, bI = 0, aI = 0;
                int rS = 0, gS = 0, bS = 0, aS = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = std::min(lh - 1, y * 2 + dy);
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(lw - 1, x * 2 + dx);
                        const QRgb pi = srcI[sy * lw + sx];
                        const QRgb ps = srcS[sy * lw + sx];
                        rI += qRed(pi); gI += qGreen(pi); bI += qBlue(pi); aI += qAlpha(pi);
                        rS += qRed(ps); gS += qGreen(ps); bS += qBlue(ps); aS += qAlpha(ps);
                    }
                }
                const int idx = y * nw + x;
                nextI[idx] = qRgba(rI / 4, gI / 4, bI / 4, aI / 4);
                nextS[idx] = qRgba(rS / 4, gS / 4, bS / 4, aS / 4);
            }
        }
        srcI = nextI;
        srcS = nextS;
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
    for (int y = 0; y < coarseH; ++y) {
        for (int x = 0; x < coarseW; ++x) {
            acc[y * coarseW + x] = levels.last().i[y * coarseW + x];
        }
    }

    // Build from the second-coarsest level down to level 0.
    for (int level = levels.size() - 2; level >= 0; --level) {
        const Level& l = levels[level];
        const int cw = width >> level;
        const int ch = height >> level;
        QVector<QRgb> up(static_cast<size_t>(cw) * ch);

        // Upsample acc (which is 2x smaller than this level) bilinear.
        const int uw = cw / 2;
        const int uh = ch / 2;
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
                const QRgb p00 = acc[y0 * uw + x0];
                const QRgb p10 = acc[y0 * uw + x1];
                const QRgb p01 = acc[y1 * uw + x0];
                const QRgb p11 = acc[y1 * uw + x1];
                auto mix = [&](double v00, double v10, double v01, double v11) {
                    return v00 * (1.0 - fx) * (1.0 - fy) + v10 * fx * (1.0 - fy)
                         + v01 * (1.0 - fx) * fy + v11 * fx * fy;
                };
                const double r = mix(qRed(p00), qRed(p10), qRed(p01), qRed(p11));
                const double g = mix(qGreen(p00), qGreen(p10), qGreen(p01), qGreen(p11));
                const double b = mix(qBlue(p00), qBlue(p10), qBlue(p01), qBlue(p11));
                const double a = mix(qAlpha(p00), qAlpha(p10), qAlpha(p01), qAlpha(p11));
                up[y * cw + x] = qRgba(static_cast<int>(std::round(r)),
                                       static_cast<int>(std::round(g)),
                                       static_cast<int>(std::round(b)),
                                       static_cast<int>(std::round(a)));
            }
        }

        // acc grows from the coarsest size to this level's size as we go up the
        // pyramid. The upsample above only read the old (2x smaller) buffer, so
        // it is safe to grow it here before writing the blended band back.
        acc.resize(static_cast<size_t>(cw) * ch);

        // Detail band = this level minus upsampled coarser content. Blend the
        // detail from "original" with the detail from "shifted" via the mask,
        // then add it onto the upsampled accumulator.
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const QRgb mi = l.mask[y * cw + x];
                const double m = qRed(mi) / 255.0; // mask value 0..1
                const QRgb pi = l.i[y * cw + x];
                const QRgb ps = l.s[y * cw + x];
                const QRgb pu = up[y * cw + x];
                auto band = [&](int p, int u) { return p - u; };
                const double dI_r = band(qRed(pi), qRed(pu));
                const double dI_g = band(qGreen(pi), qGreen(pu));
                const double dI_b = band(qBlue(pi), qBlue(pu));
                const double dI_a = band(qAlpha(pi), qAlpha(pu));
                const double dS_r = band(qRed(ps), qRed(pu));
                const double dS_g = band(qGreen(ps), qGreen(pu));
                const double dS_b = band(qBlue(ps), qBlue(pu));
                const double dS_a = band(qAlpha(ps), qAlpha(pu));
                const double blend_r = dI_r * m + dS_r * (1.0 - m);
                const double blend_g = dI_g * m + dS_g * (1.0 - m);
                const double blend_b = dI_b * m + dS_b * (1.0 - m);
                const double blend_a = dI_a * m + dS_a * (1.0 - m);
                acc[y * cw + x] = qRgba(
                    static_cast<int>(std::clamp(qRed(pu) + blend_r, 0.0, 255.0)),
                    static_cast<int>(std::clamp(qGreen(pu) + blend_g, 0.0, 255.0)),
                    static_cast<int>(std::clamp(qBlue(pu) + blend_b, 0.0, 255.0)),
                    static_cast<int>(std::clamp(qAlpha(pu) + blend_a, 0.0, 255.0)));
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

QImage TextureGenerator::createHeightMap(const QImage& image, double strength, TextureBackendMode backendMode, bool wrap)
{
    QImage gray(image.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < image.height(); ++y) {
        uchar* outLine = gray.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const double l = luminance(image.pixel(x, y));
            outLine[x] = static_cast<uchar>(saturate(l) * 255.0);
        }
    }

    // Multi-scale difference-of-Gaussians. Every blur is wrap-aware in seamless
    // mode so the height map stays tileable at its borders.
    const bool enhanced = backendMode == TextureBackendMode::EnhancedDetail;
    const QImage blurFine = boxBlurGray(gray, scaledRadius(1, gray), wrap);
    const QImage blurSmall = boxBlurGray(gray, scaledRadius(enhanced ? 3 : 2, gray), wrap);
    const QImage blurMedium = boxBlurGray(gray, scaledRadius(enhanced ? 8 : 6, gray), wrap);
    const QImage blurLarge = boxBlurGray(gray, scaledRadius(enhanced ? 20 : 12, gray), wrap);
    QImage output(gray.size(), QImage::Format_Grayscale8);

    for (int y = 0; y < gray.height(); ++y) {
        const uchar* srcLine = gray.constScanLine(y);
        const uchar* fineLine = blurFine.constScanLine(y);
        const uchar* smallLine = blurSmall.constScanLine(y);
        const uchar* mediumLine = blurMedium.constScanLine(y);
        const uchar* largeLine = blurLarge.constScanLine(y);
        uchar* outLine = output.scanLine(y);

        for (int x = 0; x < gray.width(); ++x) {
            const double luma = srcLine[x] / 255.0;
            const double micro = luma - fineLine[x] / 255.0;
            const double fine = fineLine[x] / 255.0 - smallLine[x] / 255.0;
            const double medium = smallLine[x] / 255.0 - mediumLine[x] / 255.0;
            const double large = mediumLine[x] / 255.0 - largeLine[x] / 255.0;

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

            const double centered = 0.5 + (merged - 0.5) * strength;
            outLine[x] = static_cast<uchar>(saturate(centered) * 255.0);
        }
    }

    return output;
}

QImage TextureGenerator::createNormalMap(const QImage& heightMap, double strength, TextureBackendMode backendMode, bool wrap)
{
    QImage src = heightMap.convertToFormat(QImage::Format_Grayscale8);
    QImage normal(src.size(), QImage::Format_ARGB32_Premultiplied);

    const int width = src.width();
    const int height = src.height();
    const double scale = backendMode == TextureBackendMode::EnhancedDetail ? strength * 2.35 : strength * 2.0;

    auto sample = [&](int x, int y) {
        int sx = x;
        int sy = y;
        if (wrap) {
            sx = wrapIndex(x, width);
            sy = wrapIndex(y, height);
        } else {
            sx = std::clamp(x, 0, width - 1);
            sy = std::clamp(y, 0, height - 1);
        }
        return src.constScanLine(sy)[sx] / 255.0;
    };

    for (int y = 0; y < height; ++y) {
        QRgb* outLine = reinterpret_cast<QRgb*>(normal.scanLine(y));
        for (int x = 0; x < width; ++x) {
            // Sobel 3x3 gradient; /4 keeps the strength scale comparable to the
            // previous central-difference version. Y sign stays aligned with
            // Blender's tangent-space convention (bright G = slope toward +Y).
            const double gx = (sample(x + 1, y - 1) + 2.0 * sample(x + 1, y) + sample(x + 1, y + 1))
                            - (sample(x - 1, y - 1) + 2.0 * sample(x - 1, y) + sample(x - 1, y + 1));
            const double gy = (sample(x - 1, y + 1) + 2.0 * sample(x, y + 1) + sample(x + 1, y + 1))
                            - (sample(x - 1, y - 1) + 2.0 * sample(x, y - 1) + sample(x + 1, y - 1));

            double nx = -gx * scale / 4.0;
            double ny = -gy * scale / 4.0;
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

QImage TextureGenerator::createRoughnessMap(const QImage& image, const QImage& heightMap, TextureBackendMode backendMode, bool wrap)
{
    const QImage fineBlur = boxBlurGray(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 1 : 2, heightMap), wrap);
    const QImage broadBlur = boxBlurGray(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 16 : 12, heightMap), wrap);
    QImage output(image.size(), QImage::Format_Grayscale8);

    for (int y = 0; y < image.height(); ++y) {
        const uchar* heightLine = heightMap.constScanLine(y);
        const uchar* fineLine = fineBlur.constScanLine(y);
        const uchar* broadLine = broadBlur.constScanLine(y);
        uchar* outLine = output.scanLine(y);

        for (int x = 0; x < image.width(); ++x) {
            const QColor color = QColor::fromRgba(image.pixel(x, y));
            const double luma = luminance(color.rgba());
            const double maxChannel = std::max({color.redF(), color.greenF(), color.blueF()});
            const double minChannel = std::min({color.redF(), color.greenF(), color.blueF()});
            const double saturation = maxChannel > 0.0001 ? (maxChannel - minChannel) / maxChannel : 0.0;
            const double microContrast = std::abs(heightLine[x] - fineLine[x]) / 255.0;
            const double macroContrast = std::abs(heightLine[x] - broadLine[x]) / 255.0;
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

QImage TextureGenerator::createAoMap(const QImage& image, const QImage& heightMap, TextureBackendMode backendMode, bool wrap)
{
    Q_UNUSED(image)

    const QImage midBlur = boxBlurGray(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 8 : 6, heightMap), wrap);
    const QImage largeBlur = boxBlurGray(heightMap, scaledRadius(backendMode == TextureBackendMode::EnhancedDetail ? 24 : 18, heightMap), wrap);
    QImage output(heightMap.size(), QImage::Format_Grayscale8);

    for (int y = 0; y < heightMap.height(); ++y) {
        const uchar* baseLine = heightMap.constScanLine(y);
        const uchar* midLine = midBlur.constScanLine(y);
        const uchar* largeLine = largeBlur.constScanLine(y);
        uchar* outLine = output.scanLine(y);

        for (int x = 0; x < heightMap.width(); ++x) {
            const double heightValue = baseLine[x] / 255.0;
            const double cavity = saturate((midLine[x] / 255.0 - heightValue) * (backendMode == TextureBackendMode::EnhancedDetail ? 1.6 : 1.35));
            const double basin = saturate((largeLine[x] / 255.0 - heightValue) * (backendMode == TextureBackendMode::EnhancedDetail ? 1.32 : 1.1));
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


