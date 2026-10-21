// Where the live preview's time goes.
//
// "The preview feels slow" is not a number, and the pipeline has enough stages
// that guessing which one is the cost is how an afternoon disappears. This runs
// the same source through the preview once per configuration and prints the
// wall time, so a change can be judged against a measurement rather than an
// impression.
//
// It prints rather than asserts: a timing threshold on a machine that might be
// busy is a flaky test, and a flaky test is worse than none. The numbers are the
// deliverable; the README's "preview cost" notes quote them.
#include "TextureGenerator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>

#include <cmath>
#include <cstdio>

namespace
{
// A photo-like source: several octaves of structure plus a little grain, so the
// seam cut has real edges to work with rather than a flat field it solves in
// microseconds.
QImage makePhotoLikeSource(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double value = 0.5;
            double amplitude = 0.30;
            double frequency = 3.0;
            unsigned int seed = static_cast<unsigned int>(x * 73856093u ^ y * 19349663u);
            for (int octave = 0; octave < 6; ++octave) {
                value += amplitude * std::sin(2.0 * 3.141592653589793 * frequency * x / size
                                              + 0.7 * std::sin(2.0 * 3.141592653589793 * frequency * y / size));
                amplitude *= 0.55;
                frequency *= 2.1;
            }
            seed = seed * 1664525u + 1013904223u;
            value += (static_cast<double>(seed % 1000u) / 1000.0 - 0.5) * 0.02;
            const int c = static_cast<int>(std::lround(std::max(0.0, std::min(1.0, value)) * 255.0));
            image.setPixel(x, y, qRgb(c, static_cast<int>(c * 0.92), static_cast<int>(c * 0.80)));
        }
    }
    return image;
}

TextureOptions allMaps(int resolution)
{
    TextureOptions options;
    options.resolution = resolution;
    options.square = true;
    options.seamless = true;
    options.backendMode = TextureBackendMode::EnhancedDetail;
    return options;
}

void run(const QImage& source, const char* label, TextureOptions options)
{
    // Two passes: the first pays for lazy one-off costs (Qt's image conversion
    // paths, allocator growth), the second is the number a repeat preview pays.
    QElapsedTimer timer;
    timer.start();
    TextureGenerator::preview(source, options);
    const qint64 cold = timer.elapsed();

    timer.restart();
    const TextureResult result = TextureGenerator::preview(source, options);
    const qint64 warm = timer.elapsed();

    std::printf("TIMING %-40s %5lld ms (first %5lld)  %dx%d\n", label, warm, cold,
                result.size.width(), result.size.height());
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    constexpr int kSourceSide = 2048;
    const QImage source = makePhotoLikeSource(kSourceSide);
    std::printf("TIMING source %dx%d\n", source.width(), source.height());

    run(source, "2048 full, seamless + all maps", allMaps(2048));

    {
        TextureOptions options = allMaps(2048);
        options.seamless = false;
        run(source, "2048 full, no seamless", options);
    }
    {
        TextureOptions options = allMaps(2048);
        options.generateBaseColor = false;
        options.generateBump = false;
        options.generateNormal = true;
        options.generateRoughness = false;
        options.generateMetal = false;
        options.generateAo = false;
        options.generateAlpha = false;
        run(source, "2048 normal only, seamless", options);
    }
    {
        TextureOptions options = allMaps(2048);
        options.generateBaseColor = false;
        options.generateBump = true;
        options.generateNormal = false;
        options.generateRoughness = false;
        options.generateMetal = false;
        options.generateAo = false;
        options.generateAlpha = false;
        run(source, "2048 bump only, seamless", options);
    }
    run(source, "1024 full, seamless + all maps", allMaps(1024));
    {
        // What the slider drag path actually asks for.
        TextureOptions options = allMaps(512);
        run(source, "512 (during a drag), all maps", options);
    }

    return 0;
}
