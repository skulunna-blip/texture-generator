#include "DepthEstimator.h"
#include "OnnxRuntime.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// The graph's height/width are dynamic but must be multiples of 14 (the patch
// size of the ViT backbone); the reference preprocessor_config.json asks for
// the same.
constexpr int kSideMultiple = 14;

// ImageNet statistics the checkpoint was trained with.
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

// First match wins. The fp16 export is what ships: measured against fp32 it
// holds a 0.9990 gradient correlation at ~85 MB instead of ~135 MB. int8 was
// tried and rejected - it was both 3x slower and visibly different (0.949
// gradient correlation), because dynamic quantisation's quantise/dequantise
// overhead dominates for a ViT this small.
const char* const kModelNames[] = {
    "depth_anything_v2_small_fp16.onnx",
    "depth_anything_v2_small.onnx",
    "depth_anything_v2_small_fp32.onnx",
};

// ONNX Runtime is loaded at run time rather than linked. That keeps the app
// buildable and runnable without the runtime present - the Neural backend just
// reports itself unavailable and everything falls back to the classical path.
const OrtApi* ortApi()
{
    static const OrtApi* cached = []() -> const OrtApi* {
        const QString path = DepthEstimator::findLibrary();
        if (path.isEmpty()) {
            return nullptr;
        }
        static QLibrary library(path);
        if (!library.load()) {
            qWarning("depth: cannot load %s: %s", qPrintable(path), qPrintable(library.errorString()));
            return nullptr;
        }
        using GetApiBaseFn = const OrtApiBase* (*)();
        auto getApiBase = reinterpret_cast<GetApiBaseFn>(library.resolve("OrtGetApiBase"));
        if (!getApiBase) {
            qWarning("depth: %s has no OrtGetApiBase", qPrintable(path));
            return nullptr;
        }
        const OrtApiBase* base = getApiBase();
        return base ? base->GetApi(ORT_API_VERSION) : nullptr;
    }();
    return cached;
}

struct Backend
{
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::string inputName;
    std::string outputName;
    bool ready = false;
};

// Session creation costs ~0.5 s (model load plus graph optimisation), which is
// on the order of the inference itself, so it is built once and reused. ONNX
// Runtime sessions are safe to Run() concurrently.
Backend& backend()
{
    static Backend instance = []() {
        Backend out;
        const OrtApi* api = ortApi();
        if (!api) {
            return out;
        }
        const QString modelPath = DepthEstimator::findModel();
        if (modelPath.isEmpty()) {
            return out;
        }

        OrtEnv* env = nullptr;
        if (api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "texture_generator", &env) != nullptr) {
            return out;
        }

        OrtSessionOptions* options = nullptr;
        if (api->CreateSessionOptions(&options) != nullptr) {
            return out;
        }
        // Every ORT call hands back an OrtStatus that the caller owns, even the
        // ones that cannot realistically fail.
        auto discard = [api](OrtStatus* status) {
            if (status != nullptr) {
                api->ReleaseStatus(status);
            }
        };
        // Leave one core for the UI thread; a saturated machine makes the
        // window feel frozen even though the work is on a worker thread.
        discard(api->SetIntraOpNumThreads(options, std::max(1, QThread::idealThreadCount() - 1)));
        discard(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));

        OrtSession* session = nullptr;
        // ORTCHAR_T is wchar_t on Windows, and QString::utf16() is UTF-16 there,
        // so this reinterpret is exact rather than a guess.
        const ORTCHAR_T* model = reinterpret_cast<const ORTCHAR_T*>(modelPath.utf16());
        OrtStatus* status = api->CreateSession(env, model, options, &session);
        api->ReleaseSessionOptions(options);
        if (status != nullptr) {
            qWarning("depth: cannot open %s: %s", qPrintable(modelPath), api->GetErrorMessage(status));
            api->ReleaseStatus(status);
            return out;
        }

        OrtMemoryInfo* memory = nullptr;
        if (api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory) != nullptr) {
            api->ReleaseSession(session);
            return out;
        }

        // Read the tensor names off the graph instead of hard-coding them.
        OrtAllocator* allocator = nullptr;
        char* inputName = nullptr;
        char* outputName = nullptr;
        const bool named = api->GetAllocatorWithDefaultOptions(&allocator) == nullptr
            && api->SessionGetInputName(session, 0, allocator, &inputName) == nullptr
            && api->SessionGetOutputName(session, 0, allocator, &outputName) == nullptr;
        if (!named) {
            api->ReleaseMemoryInfo(memory);
            api->ReleaseSession(session);
            return out;
        }
        out.inputName = inputName;
        out.outputName = outputName;
        discard(api->AllocatorFree(allocator, inputName));
        discard(api->AllocatorFree(allocator, outputName));

        out.api = api;
        out.env = env;
        out.session = session;
        out.memory = memory;
        out.ready = true;
        return out;
    }();
    return instance;
}

// One slot is enough: the UI only ever asks about the source it is currently
// showing, and a second image means the user switched files anyway.
struct Cache
{
    QMutex mutex;
    quint64 key = 0;
    QImage image;
};

Cache& cache()
{
    static Cache instance;
    return instance;
}

// FNV-1a over the pixel bytes. This exists so that dragging a slider - which
// regenerates the maps but not the source - hits the cache instead of paying
// another second of inference. Hashing a 4K image costs tens of milliseconds
// against a ~1 s inference, so there is no reason to risk a cheaper fingerprint
// colliding and serving a stale depth map.
quint64 fingerprint(const QImage& image, int inputLong)
{
    quint64 hash = 1469598103934665603ULL;
    auto mix = [&hash](quint64 value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    mix(static_cast<quint64>(image.width()));
    mix(static_cast<quint64>(image.height()));
    mix(static_cast<quint64>(inputLong));

    const int bytes = image.width() * 4;
    for (int y = 0; y < image.height(); ++y) {
        const uchar* line = image.constScanLine(y);
        int i = 0;
        for (; i + 8 <= bytes; i += 8) {
            quint64 word = 0;
            std::memcpy(&word, line + i, 8);
            mix(word);
        }
        for (; i < bytes; ++i) {
            mix(line[i]);
        }
    }
    return hash;
}
} // namespace

QString DepthEstimator::findLibrary()
{
    const QString overridePath = qEnvironmentVariable("TG_ONNXRUNTIME_DLL");
    if (!overridePath.isEmpty() && QFile::exists(overridePath)) {
        return QFileInfo(overridePath).absoluteFilePath();
    }

    const QString beside = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("onnxruntime.dll"));
    if (QFile::exists(beside)) {
        return beside;
    }

    // Development layout: the exe lives in app/build, the vendored runtime in
    // app/third_party/onnxruntime/bin.
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 6 && !dir.isRoot(); ++depth) {
        const QString candidate = QDir(dir.path())
            .filePath(QStringLiteral("third_party/onnxruntime/bin/onnxruntime.dll"));
        if (QFile::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
        dir.cdUp();
    }
    return QString();
}

QString DepthEstimator::findModel()
{
    const QString overridePath = qEnvironmentVariable("TG_DEPTH_MODEL");
    if (!overridePath.isEmpty() && QFile::exists(overridePath)) {
        return QFileInfo(overridePath).absoluteFilePath();
    }

    auto searchIn = [](const QString& root) -> QString {
        for (const char* name : kModelNames) {
            const QString candidate = QDir(root).filePath(QString::fromLatin1(name));
            if (QFile::exists(candidate)) {
                return QFileInfo(candidate).absoluteFilePath();
            }
        }
        return QString();
    };

    QString found = searchIn(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models")));
    if (!found.isEmpty()) {
        return found;
    }

    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 6 && !dir.isRoot(); ++depth) {
        found = searchIn(QDir(dir.path()).filePath(QStringLiteral("models")));
        if (!found.isEmpty()) {
            return found;
        }
        dir.cdUp();
    }
    return searchIn(QDir::currentPath());
}

bool DepthEstimator::isAvailable()
{
    return !findLibrary().isEmpty() && !findModel().isEmpty();
}

void DepthEstimator::clearCache()
{
    Cache& c = cache();
    QMutexLocker lock(&c.mutex);
    c.key = 0;
    c.image = QImage();
}

QImage DepthEstimator::estimate(const QImage& image, int inputLong)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return QImage();
    }
    Backend& b = backend();
    if (!b.ready) {
        return QImage();
    }

    Cache& c = cache();
    const quint64 key = fingerprint(image, inputLong);
    {
        QMutexLocker lock(&c.mutex);
        if (key == c.key && !c.image.isNull()) {
            return c.image;
        }
    }

    const int w = image.width();
    const int h = image.height();
    // Keep the aspect ratio and let the graph run at whatever multiple of 14
    // fits, rather than squashing to a square and stretching back.
    const double scale = static_cast<double>(std::max(14, inputLong)) / std::max(w, h);
    const int mw = std::max(kSideMultiple,
                            static_cast<int>(std::lround(w * scale / kSideMultiple)) * kSideMultiple);
    const int mh = std::max(kSideMultiple,
                            static_cast<int>(std::lround(h * scale / kSideMultiple)) * kSideMultiple);

    // Through ARGB32 first: `image` is premultiplied, and reading those bytes
    // as colour would darken every partially transparent pixel.
    const QImage rgb = image.convertToFormat(QImage::Format_ARGB32)
                           .convertToFormat(QImage::Format_RGB888)
                           .scaled(mw, mh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (rgb.isNull()) {
        return QImage();
    }

    const size_t plane = static_cast<size_t>(mw) * static_cast<size_t>(mh);
    std::vector<float> tensor(plane * 3);
    for (int y = 0; y < mh; ++y) {
        const uchar* line = rgb.constScanLine(y);
        for (int x = 0; x < mw; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = line[x * 3 + c] / 255.0f;
                tensor[static_cast<size_t>(c) * plane + static_cast<size_t>(y) * mw + x] =
                    (v - kMean[c]) / kStd[c];
            }
        }
    }

    const int64_t shape[4] = {1, 3, mh, mw};
    OrtValue* input = nullptr;
    if (b.api->CreateTensorWithDataAsOrtValue(b.memory,
                                              tensor.data(),
                                              tensor.size() * sizeof(float),
                                              shape,
                                              4,
                                              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                              &input) != nullptr) {
        return QImage();
    }

    const char* inputNames[1] = {b.inputName.c_str()};
    const char* outputNames[1] = {b.outputName.c_str()};
    OrtValue* output = nullptr;
    OrtStatus* status =
        b.api->Run(b.session, nullptr, inputNames, &input, 1, outputNames, 1, &output);
    b.api->ReleaseValue(input);
    if (status != nullptr) {
        qWarning("depth: inference failed: %s", b.api->GetErrorMessage(status));
        b.api->ReleaseStatus(status);
        return QImage();
    }

    OrtTensorTypeAndShapeInfo* info = nullptr;
    std::vector<int64_t> outShape;
    const float* data = nullptr;
    int ow = 0;
    int oh = 0;
    if (b.api->GetTensorTypeAndShape(output, &info) == nullptr) {
        size_t dims = 0;
        if (b.api->GetDimensionsCount(info, &dims) == nullptr && dims == 3) {
            outShape.resize(dims);
            if (b.api->GetDimensions(info, outShape.data(), dims) == nullptr) {
                oh = static_cast<int>(outShape[1]);
                ow = static_cast<int>(outShape[2]);
            }
        }
        b.api->ReleaseTensorTypeAndShapeInfo(info);
    }
    void* raw = nullptr;
    if (oh > 0 && ow > 0 && b.api->GetTensorMutableData(output, &raw) == nullptr) {
        data = static_cast<const float*>(raw);
    }
    b.api->ReleaseValue(output);
    if (data == nullptr) {
        return QImage();
    }

    const size_t count = static_cast<size_t>(ow) * static_cast<size_t>(oh);
    float lo = data[0];
    float hi = data[0];
    for (size_t i = 1; i < count; ++i) {
        lo = std::min(lo, data[i]);
        hi = std::max(hi, data[i]);
    }
    if (!(hi > lo)) {
        QImage flat(w, h, QImage::Format_Grayscale8);
        flat.fill(128);
        return flat;
    }

    // Normalise on the 1st..99th percentile rather than min..max, so one stray
    // pixel cannot squash the entire range into a couple of grey levels.
    constexpr int kBins = 2048;
    std::vector<int> histogram(kBins, 0);
    const double toBin = (kBins - 1) / static_cast<double>(hi - lo);
    for (size_t i = 0; i < count; ++i) {
        ++histogram[static_cast<int>((data[i] - lo) * toBin)];
    }
    auto percentile = [&](double p) {
        const long target = static_cast<long>(static_cast<double>(count) * p);
        long seen = 0;
        for (int i = 0; i < kBins; ++i) {
            seen += histogram[i];
            if (seen >= target) {
                return static_cast<float>(lo + i / toBin);
            }
        }
        return hi;
    };
    const float low = percentile(0.01);
    const float range = std::max(percentile(0.99) - low, 1e-6f);

    // The raw output is inverse depth - larger means nearer, confirmed against
    // the reference implementation - which is already the "bright is high"
    // convention the height map uses, so no inversion is needed here.
    QImage small(ow, oh, QImage::Format_Grayscale8);
    for (int y = 0; y < oh; ++y) {
        uchar* line = small.scanLine(y);
        for (int x = 0; x < ow; ++x) {
            const float v = (data[static_cast<size_t>(y) * ow + x] - low) / range;
            line[x] = static_cast<uchar>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
    }

    const QImage result = small.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    {
        QMutexLocker lock(&c.mutex);
        c.key = key;
        c.image = result;
    }
    return result;
}
