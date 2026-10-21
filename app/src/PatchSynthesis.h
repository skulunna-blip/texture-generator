#pragma once

#include <QImage>

// Patch-based synthesis: fills every masked pixel by copying patches from the
// unmasked part of the same image.
//
// One core, two personalities, because the difference between PatchMatch
// inpainting and GPNN is one line of the loop:
//   - vote = false  copies the single best-matching patch (PatchMatch, Barnes
//     et al. 2009). Fast, keeps the source's detail exactly, but the copy is
//     piecewise constant - seams between neighbouring patches stay visible.
//   - vote = true   averages every patch that lands on a pixel (the aggregation
//     GPNN, Granot et al. 2022, builds on). Blurrier on the first pass, and the
//     iterations put the detail back, but no patch edges survive.
// Both run coarse-to-fine when multiscale is on: at 1/16 size a whole patch
// spans a meaningful piece of structure, so the match gets the arrangement
// right, and each finer level only has to refine it. A single-scale search on a
// large hole has no such guide and tends to paste unrelated pieces together.
struct PatchSynthOptions
{
    int patchRadius = 4;   // patch is (2R+1)^2
    int iterations = 3;    // search + aggregate rounds per level
    int levels = 0;        // 0 = as deep as the size allows
    bool multiscale = true;
    bool vote = true;
    bool wrap = false;     // coordinates wrap at the borders (tiling synthesis)
};

// `mask` may be any format; a non-zero pixel is a hole. Pixels outside the mask
// come back untouched.
QImage patchSynthesis(const QImage& image, const QImage& mask, const PatchSynthOptions& options);
