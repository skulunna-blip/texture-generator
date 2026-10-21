// Regression check for the minimum-cut seam.
//
// Two things are tested, and the first one is the reason this file exists:
//
//  1. The max-flow is compared against brute force over every possible
//     labelling of small graphs. A seam is only as good as its cut being the
//     minimum one, and a wrong cut still produces a tileable image - there is
//     nothing in the output that says "this cut was 3% too expensive". The
//     brute force is the only place the correct answer is known.
//
//  2. The mask it produces has to be a single closed loop: the source region
//     must not touch the tile edge (otherwise opposite edges are no longer
//     neighbours and the tile breaks) and neither region may be split into
//     disconnected islands.
#include "SeamCut.h"

#include <QCoreApplication>
#include <QImage>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    const QByteArray tail = detail.isEmpty() ? QByteArray() : (QStringLiteral("  ") + detail).toUtf8();
    std::printf("SEAMCUT %-38s %s%s\n", what, ok ? "PASS" : "FAIL", tail.constData());
    std::fflush(stdout);
    if (!ok) {
        ++failures;
    }
}

// ---------------------------------------------------------------- brute force
struct TestEdge
{
    int u;
    int v;
    double capacity;
};

// Every labelling of the non-terminal nodes, with u on the source side and v on
// the sink side. Returns the cheapest cut, which is what the flow must equal.
double bruteForceCut(int nodes, const std::vector<TestEdge>& edges, int source, int sink)
{
    double best = 0.0;
    bool haveBest = false;
    const int free = nodes - 2;
    for (int mask = 0; mask < (1 << free); ++mask) {
        std::vector<char> side(static_cast<size_t>(nodes), 0);
        side[static_cast<size_t>(source)] = 1;
        int bit = 0;
        for (int v = 0; v < nodes; ++v) {
            if (v == source || v == sink) {
                continue;
            }
            if (mask & (1 << bit)) {
                side[static_cast<size_t>(v)] = 1;
            }
            ++bit;
        }
        double cost = 0.0;
        for (const TestEdge& edge : edges) {
            if (side[static_cast<size_t>(edge.u)] != side[static_cast<size_t>(edge.v)]) {
                cost += edge.capacity;
            }
        }
        if (!haveBest || cost < best) {
            best = cost;
            haveBest = true;
        }
    }
    return haveBest ? best : 0.0;
}

double cutOf(const std::vector<char>& side, const std::vector<TestEdge>& edges)
{
    double cost = 0.0;
    for (const TestEdge& edge : edges) {
        if (side[static_cast<size_t>(edge.u)] != side[static_cast<size_t>(edge.v)]) {
            cost += edge.capacity;
        }
    }
    return cost;
}

void checkAgainstBruteForce(int trial, int nodes, const std::vector<TestEdge>& edges, int source, int sink)
{
    SeamMaxFlow flow(nodes);
    for (const TestEdge& edge : edges) {
        flow.addEdge(edge.u, edge.v, edge.capacity);
    }
    const double value = flow.solve(source, sink);
    const double expected = bruteForceCut(nodes, edges, source, sink);

    std::vector<char> side(static_cast<size_t>(nodes), 0);
    for (int v = 0; v < nodes; ++v) {
        side[static_cast<size_t>(v)] = flow.sourceSide(v) ? 1 : 0;
    }
    const double labelled = cutOf(side, edges);

    const bool sameValue = std::abs(value - expected) < 1e-6;
    const bool sameCut = std::abs(labelled - expected) < 1e-6;
    const bool separates = side[static_cast<size_t>(source)] && !side[static_cast<size_t>(sink)];
    const QString detail = QStringLiteral("trial %1: flow %2, brute force %3, labelling %4")
                               .arg(trial)
                               .arg(value, 0, 'f', 4)
                               .arg(expected, 0, 'f', 4)
                               .arg(labelled, 0, 'f', 4);
    check(sameValue, "flow equals the brute-force min cut", detail);
    check(sameCut, "the reported labelling is that cut", detail);
    check(separates, "source and sink end up on opposite sides", detail);
}

unsigned int randomState = 12345u;
double nextRandom()
{
    randomState = randomState * 1664525u + 1013904223u;
    return static_cast<double>((randomState >> 8) % 100000u) / 100000.0;
}

// ------------------------------------------------------------------- the mask
QImage makeTestImage(int size)
{
    // Structured, not a gradient: a cut placed on noise tells you nothing.
    QImage image(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int cell = ((x / 24) * 7 + (y / 24) * 13) % 5;
            const int base = 60 + cell * 35;
            const double diagonal = 40.0 * (x + y) / (2.0 * size);
            const int v = std::max(0, std::min(255, base + static_cast<int>(diagonal)));
            image.setPixel(x, y, qRgb(v, 255 - v / 3, 90 + v / 4));
        }
    }
    return image;
}

int componentCount(const QImage& mask, bool wantSet, bool wrap)
{
    const int width = mask.width();
    const int height = mask.height();
    std::vector<char> seen(static_cast<size_t>(width) * height, 0);
    int components = 0;
    std::vector<int> stack;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int start = y * width + x;
            const bool value = mask.constScanLine(y)[x] != 0;
            if (seen[static_cast<size_t>(start)] || value != wantSet) {
                continue;
            }
            ++components;
            seen[static_cast<size_t>(start)] = 1;
            stack.push_back(start);
            while (!stack.empty()) {
                const int index = stack.back();
                stack.pop_back();
                const int px = index % width;
                const int py = index / width;
                const int neighbours[4][2] = {{px + 1, py}, {px - 1, py}, {px, py + 1}, {px, py - 1}};
                for (const auto& neighbour : neighbours) {
                    int nx = neighbour[0];
                    int ny = neighbour[1];
                    if (wrap) {
                        nx = (nx % width + width) % width;
                        ny = (ny % height + height) % height;
                    } else if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const int next = ny * width + nx;
                    if (seen[static_cast<size_t>(next)]) {
                        continue;
                    }
                    if ((mask.constScanLine(ny)[nx] != 0) != wantSet) {
                        continue;
                    }
                    seen[static_cast<size_t>(next)] = 1;
                    stack.push_back(next);
                }
            }
        }
    }
    return components;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // --- 1. Max flow against brute force ----------------------------------
    {
        int trials = 0;
        for (int i = 0; i < 6; ++i) {
            const int nodes = 9;
            std::vector<TestEdge> edges;
            // A guaranteed path from source to sink so the answer is finite, then
            // random extras and a few zero-capacity edges.
            edges.push_back({0, 8, 3.0});
            for (int k = 0; k < 16; ++k) {
                const int u = static_cast<int>(nextRandom() * nodes) % nodes;
                const int v = static_cast<int>(nextRandom() * nodes) % nodes;
                if (u == v) {
                    continue;
                }
                const double capacity = std::round(nextRandom() * 40.0) / 10.0;
                edges.push_back({u, v, capacity});
            }
            checkAgainstBruteForce(++trials, nodes, edges, 0, 8);
        }

        // A graph where every path has to go through one edge: the answer there
        // is that edge, and it is easy for an implementation to get the value
        // right while cutting somewhere else.
        {
            std::vector<TestEdge> edges = {
                {0, 1, 5.0}, {1, 2, 1.0}, {2, 3, 5.0}, {0, 3, 4.0}, {1, 3, 0.5},
            };
            checkAgainstBruteForce(++trials, 4, edges, 0, 3);
        }
    }

    // --- 2. The mask per axis ---------------------------------------------
    //
    // Each pass tiles one axis, so along that axis the shape is fixed: shifted
    // copy at both edges, source through the middle, and the cut free in the
    // two channels between. Getting that backwards produces a tile whose edges
    // match while its middle is broken, which is the failure this is here to
    // catch - it looks perfect in a 1x1 view.
    {
        const QImage image = makeTestImage(256);
        const auto checkAxis = [&](SeamAxis axis, const char* name) {
            const bool horizontal = axis == SeamAxis::Horizontal;
            const QImage mask = buildSeamlessCutMask(image, axis);
            check(!mask.isNull(), "mask built for a 256x256 source", QString::fromLatin1(name));
            if (mask.isNull()) {
                return;
            }
            check(mask.size() == image.size(), "mask matches the source size", QString::fromLatin1(name));

            const int along = horizontal ? mask.width() : mask.height();
            const int across = horizontal ? mask.height() : mask.width();
            auto at = [&](int a, int c) {
                return horizontal ? mask.constScanLine(c)[a] : mask.constScanLine(a)[c];
            };

            bool edgesShifted = true;
            bool middleSource = true;
            for (int c = 0; c < across; ++c) {
                edgesShifted = edgesShifted && at(0, c) == 0 && at(along - 1, c) == 0;
                middleSource = middleSource && at(along / 2, c) == 255;
            }
            check(edgesShifted, "both edges along the axis are shifted-copy", QString::fromLatin1(name));
            check(middleSource, "the middle band is source", QString::fromLatin1(name));

            int setPixels = 0;
            for (int y = 0; y < mask.height(); ++y) {
                for (int x = 0; x < mask.width(); ++x) {
                    setPixels += mask.constScanLine(y)[x] ? 1 : 0;
                }
            }
            const double fraction = static_cast<double>(setPixels) / (mask.width() * mask.height());
            check(fraction > 0.05 && fraction < 0.95, "both regions are substantial",
                  QStringLiteral("%1: source region %2%").arg(QString::fromLatin1(name))
                      .arg(fraction * 100.0, 0, 'f', 1));

            // The source band spans the tile, so it separates the two shifted
            // strips: one piece of source, two of shifted copy.
            check(componentCount(mask, true, false) == 1, "the source region is one piece",
                  QString::fromLatin1(name));
            check(componentCount(mask, false, false) == 2, "the shifted region is two strips",
                  QString::fromLatin1(name));

            const QImage again = buildSeamlessCutMask(image, axis);
            check(!again.isNull() && again == mask, "the same source gives the same cut",
                  QString::fromLatin1(name));
        };
        checkAxis(SeamAxis::Horizontal, "horizontal");
        checkAxis(SeamAxis::Vertical, "vertical");

        // A tile that is wide but short can tile along the long axis and not
        // along the short one. The short axis must report "no mask" rather than
        // pinning everything to one copy, and the caller then cross-fades.
        QImage wide(512, 32, QImage::Format_RGB32);
        wide.fill(qRgb(120, 90, 70));
        const QImage wideMask = buildSeamlessCutMask(wide, SeamAxis::Horizontal);
        check(!wideMask.isNull() && wideMask.size() == wide.size(),
              "a long axis still yields a full-size mask");
        check(buildSeamlessCutMask(wide, SeamAxis::Vertical).isNull(),
              "a too-short axis is refused, not faked");

        // Narrow enough and there is no free channel on either side of the
        // middle. Same rule: say so, never return a mask that is one side
        // everywhere.
        QImage narrow(24, 400, QImage::Format_RGB32);
        narrow.fill(qRgb(120, 90, 70));
        check(buildSeamlessCutMask(narrow, SeamAxis::Horizontal).isNull(),
              "a too-narrow tile is refused, not faked");
        const QImage narrowVertical = buildSeamlessCutMask(narrow, SeamAxis::Vertical);
        bool narrowHonest = !narrowVertical.isNull();
        if (narrowHonest) {
            int both = 0;
            for (int y = 0; y < narrowVertical.height(); ++y) {
                for (int x = 0; x < narrowVertical.width(); ++x) {
                    both |= narrowVertical.constScanLine(y)[x] ? 1 : 2;
                }
            }
            narrowHonest = narrowVertical.size() == narrow.size() && both == 3;
        }
        check(narrowHonest, "the long axis of the same tile still works");
    }

    std::printf("SEAMCUT %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
