#pragma once

#include <QImage>

// Removes baked-in lighting from a photo so it can serve as a proper albedo /
// base-colour map.
//
// A photograph is never pure albedo. It is albedo * shading + specular, with an
// illuminant colour cast on top. Feeding that straight into a PBR base colour
// bakes the lighting of the day into the material. This class undoes the three
// components a single image can actually be separated into:
//
//   1. specular highlights - chromaticity outliers (Yang et al., CVPR 2010)
//   2. illuminant colour   - Shades-of-Gray (p = 6) gain correction
//   3. diffuse shading     - single-scale Retinex in log space, depth-guided
//                            when a depth map is supplied
//
// This is a *classical* estimate, and it is the best one that can ship inside a
// desktop tool. Deep intrinsic decomposition (Marigold IID and friends) is
// markedly better, but the published models are ~0.9B-parameter diffusion
// networks that take about a minute per image on a datacentre GPU, so they
// cannot be bundled and cannot run on a customer's CPU.
class LightingRemoval
{
public:
    // image     Source photo. Alpha is carried through byte-exact.
    // depth     Optional normalised depth map (near = bright), same size as
    //           `image`. When non-null the shading estimate is filtered with it
    //           as the guide, which stops shading from bleeding across depth
    //           discontinuities (object silhouettes). Inside a surface, depth is
    //           flat, the filter's linear term collapses and it falls back to
    //           the plain blur used when no depth is supplied.
    // strength  0 = byte-exact passthrough, 1 = full removal.
    // wrap      Seamless mode: every filter wraps at the borders so the result
    //           stays tileable.
    static QImage remove(const QImage& image, const QImage& depth, double strength, bool wrap);

    // Rebuilds blown-out highlights only, and only on a photograph.
    //
    // "Blown" is a property of the sensor, not of the texture: a pixel that
    // railed at 255 has lost its colour entirely and has to be reinvented from
    // its surroundings. Once the seamless pass has run, the pyramid blend drags
    // those pixels down to 251, so the evidence that they were ever clipped is
    // gone and the stage has nothing to work with - the highlight survives as a
    // flat grey disc. Callers that tile must therefore run this first, on the
    // source, and let the seamless pass tile the repaired result.
    //
    // Costs one pass; a no-op unless the image actually has a plateau at its own
    // ceiling. Returns `image` unchanged when there is nothing to rebuild.
    static QImage restoreClippedHighlights(const QImage& image);

    // Self-check for the sRGB conversion tables. The sRGB <-> linear round trip
    // is 80% of this pass's runtime, so it is table-driven rather than calling
    // std::pow() per pixel; the tables are built on the 8-bit output grid so
    // the substitution is lossless rather than approximate.
    //
    // Returns the largest number of 8-bit levels by which the tables differ
    // from the exact std::pow form, sampled across the whole range. 0 means the
    // fast path is byte-identical to the slow one. Exposed so a test can keep
    // proving that instead of trusting a comment.
    static int tableErrorLevels();
};
