#pragma once

#include <QImage>
#include <QString>

// Monocular depth estimation, used as the height source for the normal, AO and
// bump maps when the Neural backend is selected.
//
// Why this matters: the classical pipeline derives height from luminance, which
// cannot tell a painted pattern from real relief - a printed stripe and a ridge
// produce the same Sobel response. A depth model sees geometry, so the normals
// it produces describe the surface rather than the paint.
//
// Model: Depth Anything V2 Small (Apache-2.0), exported to ONNX. Its output is
// *inverse* depth - verified against the reference implementation - so larger
// values are nearer, which is exactly the "bright is high" convention the
// height map already uses.
//
// Everything here degrades gracefully. When the runtime DLL or the model file
// is missing, isAvailable() reports false and estimate() returns a null image;
// callers fall back to the classical path and the app keeps working.
class DepthEstimator
{
public:
    // Absolute path of onnxruntime.dll, or an empty string. Honours the
    // TG_ONNXRUNTIME_DLL override, then looks beside the executable, then in
    // the source tree's third_party/onnxruntime/bin.
    static QString findLibrary();

    // Absolute path of the ONNX model, or an empty string. Honours
    // TG_DEPTH_MODEL, then looks in models/ beside the executable, then walks
    // up from the executable (development layout).
    static QString findModel();

    static bool isAvailable();

    // Runs the model and returns a Grayscale8 depth map sized to match `image`
    // (near = bright). `inputLong` is the longest side fed to the network; the
    // graph requires multiples of 14, so it is rounded. Use a small value
    // (~322) for live preview and the full ~518 for the final generate.
    //
    // Returns a null image if the backend is unavailable or inference fails.
    // Blocking; call it from a worker thread.
    static QImage estimate(const QImage& image, int inputLong);

    // Drops the memoised depth map. Needed only when the caller knows the
    // source changed in a way the content fingerprint cannot see.
    static void clearCache();
};
